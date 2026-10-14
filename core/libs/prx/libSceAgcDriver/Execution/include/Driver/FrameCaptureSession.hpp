#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_FRAMECAPTURESESSION_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_FRAMECAPTURESESSION_HPP

#include "prx/libSceAgcDriver/Execution/include/FrameCapture.hpp"

namespace AgcDriver::DriverDetail {

FrameCaptureRecorder* ActiveFrameCapture();
FrameCaptureShader CapturedShader(const Shader* shader);

}

#endif
