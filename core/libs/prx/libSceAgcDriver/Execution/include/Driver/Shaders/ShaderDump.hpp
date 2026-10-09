#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_SHADERDUMP_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERS_SHADERDUMP_HPP

namespace AgcDriver::DriverDetail {

struct ShaderSnapshot;

void DumpRegisteredShader(const ShaderSnapshot& snapshot);

}

#endif
