# SensorLib component for QMI8658 IMU integration
set(SOURCES
    src/SensorQMI8658.hpp
    src/SensorQMI8658.cpp
    src/SensorLib.h
    src/SensorWireHelper.cpp
    src/SensorWireHelper.h
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS src
    REQUIRES driver
)