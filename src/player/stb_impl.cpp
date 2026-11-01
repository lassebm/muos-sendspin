// The stb_image implementation, in its own translation unit (compiled with warnings off).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
// We ask for squares of at most 1024 px; this bounds what a misbehaving server's header can make
// us allocate (stb's default allows 2^24 per side).
#define STBI_MAX_DIMENSIONS 4096
#include "stb_image.h"
