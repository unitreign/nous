#pragma once

#include "nous/Application.h"
#include "nous/Input.h"
#include "nous/Runtime.h"
#include "nous/display/DrawBuffer.h"

namespace microreader {

void run_loop(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime);
void run_loop_iteration(Application& app, DrawBuffer& buf, IInputSource& input, IRuntime& runtime);

}  // namespace microreader
