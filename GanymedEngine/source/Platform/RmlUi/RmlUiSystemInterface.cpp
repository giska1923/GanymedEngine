#include "gepch.h"
#include "Platform/RmlUi/RmlUiSystemInterface.h"

#include <GLFW/glfw3.h>

namespace GanymedE {

	double RmlUiSystemInterface::GetElapsedTime()
	{
		// Same clock the rest of the platform layer reads.
		return glfwGetTime();
	}

	bool RmlUiSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
	{
		// Core logger, not the client one: this is engine-subsystem output. Script
		// and document authoring errors surface here, which is why RCSS mistakes
		// are visible at all.
		switch (type)
		{
			case Rml::Log::LT_ERROR:
			case Rml::Log::LT_ASSERT:   GE_CORE_ERROR("RmlUi: {0}", message); break;
			case Rml::Log::LT_WARNING:  GE_CORE_WARN("RmlUi: {0}", message);  break;
			case Rml::Log::LT_INFO:     GE_CORE_INFO("RmlUi: {0}", message);  break;
			default:                    GE_CORE_TRACE("RmlUi: {0}", message); break;
		}

		// true = keep going. Returning false on an assert would break into the
		// debugger, which is not wanted in a running editor.
		return true;
	}
}
