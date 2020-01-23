#!lua
local output = "./build/" .. _ACTION

solution "prosystem_solution"
   configurations { "Debug", "Release" }

project "core"
   location (output)
   kind "StaticLib"
   language "C"
   includedirs { "." }
   files { "core/**.hxx", "core/**.c" }
   buildoptions { "-Wall" }

   configuration "Debug"
      flags { "Symbols" }
      defines { "DEBUG" }

   configuration "Release"
      flags { "Optimize" }
      defines { "NDEBUG" }

project "prosystem"
   location (output)
   kind "ConsoleApp"
   language "C"
   includedirs { "./core", "/usr/include/libdrm" }
   files { "./*.h", "./*.c"}
   buildoptions { "-Wall" }
   linkoptions { "-lpthread -lm -lrga -lgo2" }
   links {"core"}

   configuration "Debug"
      flags { "Symbols" }
      defines { "DEBUG" }

   configuration "Release"
      flags { "Optimize" }
      defines { "NDEBUG" }
