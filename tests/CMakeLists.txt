add_executable(voice_input_tests
    test_main.cpp
    ../src/addon/vad/vad.cpp
    ../src/addon/vad/silero_vad.cpp
    ../src/addon/asr/utils/base64.cpp)

target_include_directories(voice_input_tests PRIVATE
    ../src/addon
    ${FCITX5_INCLUDE_DIRS})

target_compile_definitions(voice_input_tests PRIVATE
    VOICE_INPUT_MODEL_DIR="/nonexistent"
    FCITX_GETTEXT_DOMAIN="fcitx5-voice-input")

target_link_libraries(voice_input_tests PRIVATE ${FCITX5_LIBRARIES})

add_test(NAME voice_input_tests COMMAND voice_input_tests)
