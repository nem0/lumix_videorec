-- Runs inside the plugin project. Paths are relative to this file.
-- external/nvenc/nvEncodeAPI.h is from FFmpeg's nv-codec-headers (MIT), the encoder library itself is loaded from the NVIDIA driver at runtime
files { "src/**.cpp", "src/**.h" }
includedirs { "external" }
defines { "BUILDING_VIDEOREC" }

-- plugins link each other only in dynamic builds
if _OPTIONS["dynamic-plugins"] then
	links { "core", "engine", "renderer" }
end
