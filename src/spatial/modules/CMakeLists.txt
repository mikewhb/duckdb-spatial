add_subdirectory(main)

if(SPATIAL_USE_PROJ)
  add_subdirectory(proj)
endif()

if(SPATIAL_USE_GDAL)
  add_subdirectory(gdal)
endif()

if(SPATIAL_USE_GEOS)
  add_subdirectory(geos)
endif()

if(SPATIAL_USE_OSM)
  add_subdirectory(osm)
endif()

if(SPATIAL_USE_SHAPEFILE)
  add_subdirectory(shapefile)
endif()

if(SPATIAL_USE_MVT)
  add_subdirectory(mvt)
endif()

set(EXTENSION_SOURCES
    ${EXTENSION_SOURCES}
    PARENT_SCOPE)
