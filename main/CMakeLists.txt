idf_component_register(
    SRCS "main.c" "llm_runtime.c" "gpio_control.c" "mouth.c" "voice_clips.c"
    INCLUDE_DIRS "."
    # The mouth's vocabulary (559 KB of PCM) is embedded into the app image
    # rather than given its own partition: it belongs to this firmware version
    # exactly like a table of strings does, and EMBED_FILES keeps it in .rodata
    # so playback streams straight out of flash with no RAM copy.
    EMBED_FILES "voice_clips.bin"
)
