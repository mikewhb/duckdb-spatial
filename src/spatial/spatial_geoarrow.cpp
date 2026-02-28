#include "spatial/spatial_geoarrow.hpp"

#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/schema_metadata.hpp"
#include "duckdb/function/table/arrow/arrow_duck_schema.hpp"
#include "duckdb/function/table/arrow/arrow_type_info.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "geometry/geometry_serialization.hpp"
#include "spatial/geometry/geometry_type.hpp"
#include "spatial/geometry/sgl.hpp"
#include "spatial/geometry/wkb_writer.hpp"
#include "spatial/spatial_types.hpp"
#include "yyjson.h"

namespace duckdb {

namespace {

// ============================================================
// Helper: Build ArrowStructInfo from ArrowSchema children.
// This is needed because GetType replaces the default ArrowType
// (which includes proper children info) with our custom one.
// We must rebuild the children info for ColumnArrowToDuckDB.
// ============================================================
static unique_ptr<ArrowStructInfo> BuildStructInfoFromSchema(DBConfig &config, const ArrowSchema &schema) {
	vector<shared_ptr<ArrowType>> children;
	for (idx_t i = 0; i < static_cast<idx_t>(schema.n_children); i++) {
		children.emplace_back(ArrowType::GetArrowLogicalType(config, *schema.children[i]));
	}
	return make_uniq<ArrowStructInfo>(std::move(children));
}

// ============================================================
// Helper: Build ArrowListInfo from ArrowSchema list child.
// ============================================================
static unique_ptr<ArrowListInfo> BuildListInfoFromSchema(DBConfig &config, const ArrowSchema &schema,
                                                         ArrowVariableSizeType size_type) {
	auto child_type = ArrowType::GetArrowLogicalType(config, *schema.children[0]);
	return ArrowListInfo::List(std::move(child_type), size_type);
}

// ============================================================
// Helper: Validate GeoArrow extension metadata JSON (shared)
// ============================================================
static void ValidateGeoArrowMetadata(const ArrowSchemaMetadata &schema_metadata) {
	string extension_metadata = schema_metadata.GetOption(ArrowSchemaMetadata::ARROW_METADATA_KEY);
	if (!extension_metadata.empty()) {
		using namespace duckdb_yyjson_spatial;

		unique_ptr<yyjson_doc, void (*)(yyjson_doc *)> doc(
		    yyjson_read(extension_metadata.data(), extension_metadata.size(), YYJSON_READ_NOFLAG), yyjson_doc_free);
		if (!doc) {
			throw SerializationException("Invalid JSON in GeoArrow metadata");
		}

		yyjson_val *val = yyjson_doc_get_root(doc.get());
		if (!yyjson_is_obj(val)) {
			throw SerializationException("Invalid GeoArrow metadata: not a JSON object");
		}

		yyjson_val *edges = yyjson_obj_get(val, "edges");
		if (edges && yyjson_is_str(edges) && std::strcmp(yyjson_get_str(edges), "planar") != 0) {
			throw NotImplementedException("Can't import non-planar edges");
		}
	}
}

// ============================================================
// Helper: Set GeoArrow extension metadata on an ArrowSchema
// ============================================================
static void SetGeoArrowMetadata(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema,
                                const string &extension_name) {
	ArrowSchemaMetadata schema_metadata;
	schema_metadata.AddOption(ArrowSchemaMetadata::ARROW_EXTENSION_NAME, extension_name);
	schema_metadata.AddOption(ArrowSchemaMetadata::ARROW_METADATA_KEY, "{}");
	root_holder.metadata_info.emplace_back(schema_metadata.SerializeMetadata());
	schema.metadata = root_holder.metadata_info.back().get();
}

// ============================================================
// Helper: Build Struct children in an ArrowSchema
// Constructs {x:DOUBLE, y:DOUBLE[, z:DOUBLE[, m:DOUBLE]]}
// ============================================================
static void BuildCoordStructSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema,
                                   const vector<string> &field_names) {
	schema.format = "+s";
	schema.n_children = NumericCast<int64_t>(field_names.size());
	root_holder.nested_children.emplace_back();
	root_holder.nested_children.back().resize(field_names.size());
	root_holder.nested_children_ptr.emplace_back();
	root_holder.nested_children_ptr.back().resize(field_names.size());
	for (idx_t i = 0; i < field_names.size(); i++) {
		root_holder.nested_children_ptr.back()[i] = &root_holder.nested_children.back()[i];
	}
	schema.children = &root_holder.nested_children_ptr.back()[0];
	for (idx_t i = 0; i < field_names.size(); i++) {
		auto &child = *schema.children[i];
		child.format = "g"; // DOUBLE
		child.name = nullptr;
		child.metadata = nullptr;
		child.flags = ARROW_FLAG_NULLABLE;
		child.n_children = 0;
		child.children = nullptr;
		child.dictionary = nullptr;
		child.release = nullptr;
		child.private_data = nullptr;
		// Set field name
		auto name_ptr = make_unsafe_uniq_array<char>(field_names[i].size() + 1);
		memcpy(name_ptr.get(), field_names[i].c_str(), field_names[i].size() + 1);
		root_holder.owned_type_names.push_back(std::move(name_ptr));
		child.name = root_holder.owned_type_names.back().get();
	}
}

// ============================================================
// Helper: Get coordinate field names from DuckDB LogicalType
// ============================================================
static vector<string> GetCoordFieldNames(const LogicalType &type) {
	auto &child_types = StructType::GetChildTypes(type);
	vector<string> names;
	for (auto &kv : child_types) {
		names.push_back(kv.first);
	}
	return names;
}

// ============================================================
// Helper: Get coordinate field names for innermost struct of a
// LIST or LIST<LIST<...>> type
// ============================================================
static vector<string> GetInnerCoordFieldNames(const LogicalType &type) {
	auto inner = &type;
	// Unwrap LIST layers to get to the innermost STRUCT
	while (inner->id() == LogicalTypeId::LIST) {
		inner = &ListType::GetChildType(*inner);
	}
	return GetCoordFieldNames(*inner);
}

// ============================================================
// 1. geoarrow.wkb  <-->  GEOMETRY
//    (existing, unchanged)
// ============================================================
struct GeoArrowWKB {
	static unique_ptr<ArrowType> GetType(const ArrowSchema &schema, const ArrowSchemaMetadata &schema_metadata) {
		ValidateGeoArrowMetadata(schema_metadata);

		const auto format = string(schema.format);
		if (format == "z") {
			return make_uniq<ArrowType>(GeoTypes::GEOMETRY(),
			                            make_uniq<ArrowStringInfo>(ArrowVariableSizeType::NORMAL));
		} else if (format == "Z") {
			return make_uniq<ArrowType>(GeoTypes::GEOMETRY(),
			                            make_uniq<ArrowStringInfo>(ArrowVariableSizeType::SUPER_SIZE));
		} else if (format == "vz") {
			return make_uniq<ArrowType>(GeoTypes::GEOMETRY(), make_uniq<ArrowStringInfo>(ArrowVariableSizeType::VIEW));
		}
		throw InvalidInputException("Arrow extension type \"%s\" not supported for geoarrow.wkb", format.c_str());
	}

	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		SetGeoArrowMetadata(root_holder, schema, "geoarrow.wkb");

		const auto options = context.GetClientProperties();
		if (options.arrow_offset_size == ArrowOffsetSize::LARGE) {
			schema.format = "Z";
		} else {
			schema.format = "z";
		}
	}

	static void ArrowToDuck(ClientContext &context, Vector &source, Vector &result, idx_t count) {
		ArenaAllocator arena(Allocator::Get(context));
		GeometryAllocator alloc(arena);

		sgl::wkb_reader reader(alloc);
		reader.set_allow_mixed_zm(true);
		reader.set_nan_as_empty(true);

		UnaryExecutor::ExecuteWithNulls<string_t, string_t>(
		    source, result, count, [&](const string_t &wkb, ValidityMask &mask, idx_t idx) {
			    const auto wkb_ptr = wkb.GetDataUnsafe();
			    const auto wkb_len = wkb.GetSize();

			    sgl::geometry geom;

			    if (!reader.try_parse(geom, wkb_ptr, wkb_len)) {
				    const auto error = reader.get_error_message();
				    throw InvalidInputException("Could not parse WKB input: %s", error);
			    }

			    if (reader.parsed_mixed_zm()) {
				    sgl::ops::force_zm(alloc, geom, reader.parsed_any_z(), reader.parsed_any_m(), 0, 0);
			    }

			    const auto size = Serde::GetRequiredSize(geom);
			    auto blob = StringVector::EmptyString(result, size);
			    Serde::Serialize(geom, blob.GetDataWriteable(), size);
			    blob.Finalize();
			    return blob;
		    });
	}

	static void DuckToArrow(ClientContext &context, Vector &source, Vector &result, idx_t count) {
		WKBWriter writer;
		UnaryExecutor::Execute<geometry_t, string_t>(
		    source, result, count, [&](const geometry_t &input) { return writer.Write(input, result); });
	}
};

// ============================================================
// 2. geoarrow.wkt  <-->  WKB_BLOB (as geoarrow.wkb)
//    WKB_BLOB is already standard WKB, no data conversion needed.
//    On write: inject geoarrow.wkb metadata (same as GEOMETRY).
//    On read: geoarrow.wkb is already handled by GeoArrowWKB
//    and returns GEOMETRY (the universal type).
//    So WKB_BLOB only needs a write-path registration.
// ============================================================

// ============================================================
// 3. geoarrow.point  <-->  POINT_2D / POINT_3D / POINT_4D
//    Physical layout is identical (Struct<x,y[,z[,m]]>).
//    Zero-copy: no data conversion needed.
// ============================================================
struct GeoArrowPoint {
	// Read path: Arrow geoarrow.point → DuckDB POINT_2D/3D/4D
	static unique_ptr<ArrowType> GetType(const ArrowSchema &schema, const ArrowSchemaMetadata &schema_metadata) {
		ValidateGeoArrowMetadata(schema_metadata);

		const auto format = string(schema.format);
		if (format != "+s") {
			throw InvalidInputException("Arrow extension type \"%s\" not supported for geoarrow.point", format.c_str());
		}

		// Build ArrowStructInfo from schema children (required for ColumnArrowToDuckDB)
		auto *dbconfig = ArrowType::GetCurrentDBConfig();
		D_ASSERT(dbconfig);
		auto struct_info = BuildStructInfoFromSchema(*dbconfig, schema);

		// Determine dimension from number of struct children
		switch (schema.n_children) {
		case 2:
			return make_uniq<ArrowType>(GeoTypes::POINT_2D(), std::move(struct_info));
		case 3:
			return make_uniq<ArrowType>(GeoTypes::POINT_3D(), std::move(struct_info));
		case 4:
			return make_uniq<ArrowType>(GeoTypes::POINT_4D(), std::move(struct_info));
		default:
			throw InvalidInputException("geoarrow.point has unexpected number of children: %d", schema.n_children);
		}
	}

	// Write path: DuckDB POINT_2D/3D/4D → Arrow geoarrow.point
	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		SetGeoArrowMetadata(root_holder, schema, "geoarrow.point");
		auto field_names = GetCoordFieldNames(type);
		BuildCoordStructSchema(root_holder, schema, field_names);
	}
};

// ============================================================
// 4. geoarrow.linestring  <-->  LINESTRING_2D / LINESTRING_3D
//    Physical layout: List<Struct<x,y[,z]>>
//    Zero-copy: no data conversion needed.
// ============================================================
struct GeoArrowLineString {
	// Read path: Arrow geoarrow.linestring → DuckDB LINESTRING_2D/3D
	static unique_ptr<ArrowType> GetType(const ArrowSchema &schema, const ArrowSchemaMetadata &schema_metadata) {
		ValidateGeoArrowMetadata(schema_metadata);

		const auto format = string(schema.format);
		if (format != "+l" && format != "+L") {
			throw InvalidInputException("Arrow extension type \"%s\" not supported for geoarrow.linestring",
			                            format.c_str());
		}

		// The list child should be a struct; determine dimension from its children
		if (schema.n_children != 1 || !schema.children[0]) {
			throw InvalidInputException("geoarrow.linestring must have exactly 1 child (struct)");
		}

		// Build ArrowListInfo from schema children
		auto *dbconfig = ArrowType::GetCurrentDBConfig();
		D_ASSERT(dbconfig);
		auto size_type = (format == "+L") ? ArrowVariableSizeType::SUPER_SIZE : ArrowVariableSizeType::NORMAL;
		auto list_info = BuildListInfoFromSchema(*dbconfig, schema, size_type);

		auto &coord_schema = *schema.children[0];
		switch (coord_schema.n_children) {
		case 2:
			return make_uniq<ArrowType>(GeoTypes::LINESTRING_2D(), std::move(list_info));
		case 3:
			return make_uniq<ArrowType>(GeoTypes::LINESTRING_3D(), std::move(list_info));
		default:
			throw InvalidInputException("geoarrow.linestring coord struct has unexpected dimension: %d",
			                            coord_schema.n_children);
		}
	}

	// Write path: DuckDB LINESTRING_2D/3D → Arrow geoarrow.linestring
	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		SetGeoArrowMetadata(root_holder, schema, "geoarrow.linestring");

		// Build List<Struct<x,y[,z]>>
		auto options = context.GetClientProperties();
		if (options.arrow_offset_size == ArrowOffsetSize::LARGE) {
			schema.format = "+L";
		} else {
			schema.format = "+l";
		}

		schema.n_children = 1;
		root_holder.nested_children.emplace_back();
		root_holder.nested_children.back().resize(1);
		root_holder.nested_children_ptr.emplace_back();
		root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
		schema.children = &root_holder.nested_children_ptr.back()[0];

		auto &child = root_holder.nested_children.back()[0];
		child.format = nullptr;
		child.name = nullptr;
		child.metadata = nullptr;
		child.flags = ARROW_FLAG_NULLABLE;
		child.n_children = 0;
		child.children = nullptr;
		child.dictionary = nullptr;
		child.release = nullptr;
		child.private_data = nullptr;

		// Set child name
		auto name_ptr = make_unsafe_uniq_array<char>(2);
		name_ptr[0] = 'l';
		name_ptr[1] = '\0';
		root_holder.owned_type_names.push_back(std::move(name_ptr));
		child.name = root_holder.owned_type_names.back().get();

		// Build the coord struct as the list's child
		auto field_names = GetInnerCoordFieldNames(type);
		BuildCoordStructSchema(root_holder, child, field_names);
	}
};

// ============================================================
// 5. geoarrow.polygon  <-->  POLYGON_2D / POLYGON_3D
//    Physical layout: List<List<Struct<x,y[,z]>>>
//    Zero-copy: no data conversion needed.
// ============================================================
struct GeoArrowPolygon {
	// Read path: Arrow geoarrow.polygon → DuckDB POLYGON_2D/3D
	static unique_ptr<ArrowType> GetType(const ArrowSchema &schema, const ArrowSchemaMetadata &schema_metadata) {
		ValidateGeoArrowMetadata(schema_metadata);

		const auto format = string(schema.format);
		if (format != "+l" && format != "+L") {
			throw InvalidInputException("Arrow extension type \"%s\" not supported for geoarrow.polygon",
			                            format.c_str());
		}

		// Outer list → inner list → coord struct
		if (schema.n_children != 1 || !schema.children[0]) {
			throw InvalidInputException("geoarrow.polygon must have exactly 1 child (ring list)");
		}
		auto &ring_schema = *schema.children[0];
		auto ring_format = string(ring_schema.format);
		if (ring_format != "+l" && ring_format != "+L") {
			throw InvalidInputException("geoarrow.polygon ring child must be a list, got \"%s\"", ring_format.c_str());
		}
		if (ring_schema.n_children != 1 || !ring_schema.children[0]) {
			throw InvalidInputException("geoarrow.polygon ring list must have exactly 1 child (coord struct)");
		}

		// Build ArrowListInfo from schema children (outer list)
		auto *dbconfig = ArrowType::GetCurrentDBConfig();
		D_ASSERT(dbconfig);
		auto size_type = (format == "+L") ? ArrowVariableSizeType::SUPER_SIZE : ArrowVariableSizeType::NORMAL;
		auto list_info = BuildListInfoFromSchema(*dbconfig, schema, size_type);

		auto &coord_schema = *ring_schema.children[0];
		switch (coord_schema.n_children) {
		case 2:
			return make_uniq<ArrowType>(GeoTypes::POLYGON_2D(), std::move(list_info));
		case 3:
			return make_uniq<ArrowType>(GeoTypes::POLYGON_3D(), std::move(list_info));
		default:
			throw InvalidInputException("geoarrow.polygon coord struct has unexpected dimension: %d",
			                            coord_schema.n_children);
		}
	}

	// Write path: DuckDB POLYGON_2D/3D → Arrow geoarrow.polygon
	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		SetGeoArrowMetadata(root_holder, schema, "geoarrow.polygon");

		auto options = context.GetClientProperties();
		const char *list_format = (options.arrow_offset_size == ArrowOffsetSize::LARGE) ? "+L" : "+l";

		// Build outer list: List<...>
		schema.format = list_format;
		schema.n_children = 1;
		root_holder.nested_children.emplace_back();
		root_holder.nested_children.back().resize(1);
		root_holder.nested_children_ptr.emplace_back();
		root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
		schema.children = &root_holder.nested_children_ptr.back()[0];

		auto &ring_child = root_holder.nested_children.back()[0];
		ring_child.format = nullptr;
		ring_child.name = nullptr;
		ring_child.metadata = nullptr;
		ring_child.flags = ARROW_FLAG_NULLABLE;
		ring_child.n_children = 0;
		ring_child.children = nullptr;
		ring_child.dictionary = nullptr;
		ring_child.release = nullptr;
		ring_child.private_data = nullptr;

		auto name_ptr1 = make_unsafe_uniq_array<char>(2);
		name_ptr1[0] = 'l';
		name_ptr1[1] = '\0';
		root_holder.owned_type_names.push_back(std::move(name_ptr1));
		ring_child.name = root_holder.owned_type_names.back().get();

		// Build inner list: List<Struct<...>>
		ring_child.format = list_format;
		ring_child.n_children = 1;
		root_holder.nested_children.emplace_back();
		root_holder.nested_children.back().resize(1);
		root_holder.nested_children_ptr.emplace_back();
		root_holder.nested_children_ptr.back().push_back(&root_holder.nested_children.back()[0]);
		ring_child.children = &root_holder.nested_children_ptr.back()[0];

		auto &coord_child = root_holder.nested_children.back()[0];
		coord_child.format = nullptr;
		coord_child.name = nullptr;
		coord_child.metadata = nullptr;
		coord_child.flags = ARROW_FLAG_NULLABLE;
		coord_child.n_children = 0;
		coord_child.children = nullptr;
		coord_child.dictionary = nullptr;
		coord_child.release = nullptr;
		coord_child.private_data = nullptr;

		auto name_ptr2 = make_unsafe_uniq_array<char>(2);
		name_ptr2[0] = 'l';
		name_ptr2[1] = '\0';
		root_holder.owned_type_names.push_back(std::move(name_ptr2));
		coord_child.name = root_holder.owned_type_names.back().get();

		// Build the coord struct
		auto field_names = GetInnerCoordFieldNames(type);
		BuildCoordStructSchema(root_holder, coord_child, field_names);
	}
};

// ============================================================
// 6. geoarrow.box  <-->  BOX_2D
//    DuckDB: Struct{min_x, min_y, max_x, max_y: DOUBLE}
//    GeoArrow: Struct{xmin, ymin, xmax, ymax: DOUBLE}
//    Field name renaming needed, but data buffers are positional
//    so no data conversion is needed.
// ============================================================
struct GeoArrowBox {
	// Read path: Arrow geoarrow.box → DuckDB BOX_2D
	static unique_ptr<ArrowType> GetType(const ArrowSchema &schema, const ArrowSchemaMetadata &schema_metadata) {
		ValidateGeoArrowMetadata(schema_metadata);

		const auto format = string(schema.format);
		if (format != "+s") {
			throw InvalidInputException("Arrow extension type \"%s\" not supported for geoarrow.box", format.c_str());
		}

		if (schema.n_children == 4) {
			// Build ArrowStructInfo from schema children
			auto *dbconfig = ArrowType::GetCurrentDBConfig();
			D_ASSERT(dbconfig);
			auto struct_info = BuildStructInfoFromSchema(*dbconfig, schema);
			return make_uniq<ArrowType>(GeoTypes::BOX_2D(), std::move(struct_info));
		}
		throw InvalidInputException("geoarrow.box has unexpected number of children: %d", schema.n_children);
	}

	// Write path: DuckDB BOX_2D → Arrow geoarrow.box
	static void PopulateSchema(DuckDBArrowSchemaHolder &root_holder, ArrowSchema &schema, const LogicalType &type,
	                           ClientContext &context, const ArrowTypeExtension &extension) {
		SetGeoArrowMetadata(root_holder, schema, "geoarrow.box");

		// Use GeoArrow field names: xmin, ymin, xmax, ymax
		// (DuckDB uses min_x, min_y, max_x, max_y)
		vector<string> field_names = {"xmin", "ymin", "xmax", "ymax"};
		BuildCoordStructSchema(root_holder, schema, field_names);
	}
};

// ============================================================
// Helper: Add extra type_to_info mapping for a DuckDB type
// to an existing ArrowExtensionMetadata.
// This allows multiple DuckDB types (e.g., POINT_2D, POINT_3D)
// to share the same geoarrow.point extension.
// ============================================================
static void AddExtraTypeMapping(DBConfig &config, const LogicalType &type, const ArrowExtensionMetadata &info) {
	config.RegisterArrowExtensionAlias(type, info);
}

// ============================================================
// Registration: Register all GeoArrow ArrowTypeExtensions
// ============================================================
void RegisterArrowExtensions(DBConfig &config) {
	// --- 1. geoarrow.wkb <--> GEOMETRY ---
	// (existing, complete roundtrip with WKB serialization)
	config.RegisterArrowExtension(
	    {"geoarrow.wkb", GeoArrowWKB::PopulateSchema, GeoArrowWKB::GetType,
	     make_shared_ptr<ArrowTypeExtensionData>(GeoTypes::GEOMETRY(), LogicalType::BLOB, GeoArrowWKB::ArrowToDuck,
	                                             GeoArrowWKB::DuckToArrow)});

	// --- 2. WKB_BLOB write path ---
	// WKB_BLOB is already standard WKB binary, so on write we just inject
	// geoarrow.wkb metadata (no DuckToArrow callback needed, the default
	// BLOB export produces the correct binary format).
	// On read, geoarrow.wkb is handled by GeoArrowWKB above → returns GEOMETRY
	// (WKB_BLOB and GEOMETRY share the same storage representation).
	// We register WKB_BLOB as a separate type_to_info entry pointing to
	// the same geoarrow.wkb extension.
	{
		ArrowExtensionMetadata wkb_info("geoarrow.wkb", {}, {}, {});
		AddExtraTypeMapping(config, GeoTypes::WKB_BLOB(), wkb_info);
	}

	// --- 3. geoarrow.point <--> POINT_2D / POINT_3D / POINT_4D ---
	// Zero-copy: physical layout is identical (Struct<x,y[,z[,m]]>).
	// Register once with POINT_2D as primary type, add extra mappings for 3D/4D.
	auto point_ext_data = make_shared_ptr<ArrowTypeExtensionData>(GeoTypes::POINT_2D());
	config.RegisterArrowExtension(
	    {"geoarrow.point", GeoArrowPoint::PopulateSchema, GeoArrowPoint::GetType, point_ext_data});
	{
		ArrowExtensionMetadata point_info("geoarrow.point", {}, {}, {});
		AddExtraTypeMapping(config, GeoTypes::POINT_3D(), point_info);
		AddExtraTypeMapping(config, GeoTypes::POINT_4D(), point_info);
	}

	// --- 4. geoarrow.linestring <--> LINESTRING_2D / LINESTRING_3D ---
	// Zero-copy: List<Struct<x,y[,z]>>
	auto linestring_ext_data = make_shared_ptr<ArrowTypeExtensionData>(GeoTypes::LINESTRING_2D());
	config.RegisterArrowExtension(
	    {"geoarrow.linestring", GeoArrowLineString::PopulateSchema, GeoArrowLineString::GetType,
	     linestring_ext_data});
	{
		ArrowExtensionMetadata ls_info("geoarrow.linestring", {}, {}, {});
		AddExtraTypeMapping(config, GeoTypes::LINESTRING_3D(), ls_info);
	}

	// --- 5. geoarrow.polygon <--> POLYGON_2D / POLYGON_3D ---
	// Zero-copy: List<List<Struct<x,y[,z]>>>
	auto polygon_ext_data = make_shared_ptr<ArrowTypeExtensionData>(GeoTypes::POLYGON_2D());
	config.RegisterArrowExtension(
	    {"geoarrow.polygon", GeoArrowPolygon::PopulateSchema, GeoArrowPolygon::GetType, polygon_ext_data});
	{
		ArrowExtensionMetadata pg_info("geoarrow.polygon", {}, {}, {});
		AddExtraTypeMapping(config, GeoTypes::POLYGON_3D(), pg_info);
	}

	// --- 6. geoarrow.box <--> BOX_2D ---
	// Field name renaming only (min_x↔xmin, etc.), data is positional.
	auto box_ext_data = make_shared_ptr<ArrowTypeExtensionData>(GeoTypes::BOX_2D());
	config.RegisterArrowExtension(
	    {"geoarrow.box", GeoArrowBox::PopulateSchema, GeoArrowBox::GetType, box_ext_data});

	// NOTE: BOX_2DF is NOT registered because GeoArrow spec does not define
	// a float32 box type. BOX_2DF will fall through to default STRUCT handling.
}

class GeoArrowRegisterFunctionData final : public TableFunctionData {
public:
	GeoArrowRegisterFunctionData() : finished(false) {
	}
	bool finished {false};
};

unique_ptr<FunctionData> GeoArrowRegisterBind(ClientContext &context, TableFunctionBindInput &input,
                                              vector<LogicalType> &return_types, vector<string> &names) {
	names.push_back("registered");
	return_types.push_back(LogicalType::BOOLEAN);
	return make_uniq<GeoArrowRegisterFunctionData>();
}

void GeoArrowRegisterScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.bind_data->CastNoConst<GeoArrowRegisterFunctionData>();
	if (data.finished) {
		return;
	}

	DBConfig &config = DatabaseInstance::GetDatabase(context).config;
	if (config.HasArrowExtension(GeoTypes::GEOMETRY())) {
		output.SetValue(0, 0, false);
	} else {
		RegisterArrowExtensions(config);
		output.SetValue(0, 0, true);
	}

	output.SetCardinality(1);
	data.finished = true;
}

} // namespace

void GeoArrow::Register(ExtensionLoader &loader) {
	// Automatically register all GeoArrow ArrowTypeExtensions so that
	// Arrow-based storage backends (e.g. Lance) can round-trip spatial columns
	// (GEOMETRY, POINT_2D/3D/4D, LINESTRING_2D/3D, POLYGON_2D/3D, BOX_2D)
	// with proper extension metadata without requiring a manual call to
	// register_geoarrow_extensions().
	auto &instance = loader.GetDatabaseInstance();
	DBConfig &config = DBConfig::GetConfig(instance);
	if (!config.HasArrowExtension(GeoTypes::GEOMETRY())) {
		RegisterArrowExtensions(config);
	}

	// Keep the table function for backward compatibility and explicit re-registration.
	TableFunction register_func("register_geoarrow_extensions", {}, GeoArrowRegisterScan, GeoArrowRegisterBind);
	loader.RegisterFunction(register_func);
}

} // namespace duckdb
