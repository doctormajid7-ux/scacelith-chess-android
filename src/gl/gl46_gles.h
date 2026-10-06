// OpenGL ES 3.2 backend (src/gl/gl46_gles.cpp): installs the emulations of the desktop-GL entry
// points GL ES 3.2 does not have, over the entry points the game loaded from the driver.
//
// Called once by the Android platform layer, right after gl46::load() and before
// gl46::afterContextCreated() (the game's first GL call).
#pragma once

namespace gl46 {

// Fills the entry points the driver could not resolve with the ES emulations of
// gl46_gles.cpp. Resolves the classic ES entry points it needs from libGLESv2.so itself.
// Does nothing (and logs) when the ES library cannot be opened.
void installGlesFallbacks();

// Whether the emulation layer is in place (the ES library was found).
bool glesReady();

// True when the driver has no GL_EXT_clip_control: glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)
// cannot be honoured by the driver, and every vertex stage must remap its clip z from the [0, 1]
// convention to ES's [-1, 1] itself (src/render/shader.cpp does it while preprocessing).
bool glesRemapDepth();

// Whether the ES driver lists the extension (GL_EXTENSIONS through glGetStringi).
bool glesHasExtension(const char* name);

// Forgets the texture targets and the bindings the emulation tracks, so a caller can start from a
// state it knows. The game never calls this: it is tests/gles_tests.cpp's way back to a clean
// layer, and the reason the unit tests need no reset of the whole process.
void resetGlesFallbacks();

}  // namespace gl46
