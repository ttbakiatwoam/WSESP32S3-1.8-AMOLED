# SensorLib component for QMI8658 IMU integration
set(SOURCES
    src/SensorWireHelper.cpp
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS src
    REQUIRES driver
)