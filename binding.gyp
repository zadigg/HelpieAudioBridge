{
  "targets": [
    {
      "target_name": "audio_capture",
      "sources": [
        "wrapper.cc",
        "audio_capture.mm"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS=0",
        "NAPI_VERSION=8"
      ],
      "cflags!": ["-fno-exceptions"],
      "cflags_cc!": ["-fno-exceptions"],
      "xcode_settings": {
        "GCC_ENABLE_CPP_EXCEPTIONS": "YES",
        "CLANG_CXX_LIBRARY": "libc++",
        "MACOSX_DEPLOYMENT_TARGET": "13.0",
        "CLANG_ENABLE_OBJC_ARC": "YES",
        "OTHER_LDFLAGS": [
          "-framework ScreenCaptureKit",
          "-framework AVFoundation",
          "-framework CoreAudio",
          "-framework AudioToolbox",
          "-framework Foundation"
        ],
        "OTHER_CFLAGS": [
          "-fobjc-arc"
        ]
      },
      "link_settings": {
        "libraries": [
          "-framework ScreenCaptureKit",
          "-framework AVFoundation",
          "-framework CoreAudio",
          "-framework AudioToolbox",
          "-framework Foundation"
        ]
      }
    }
  ]
}
