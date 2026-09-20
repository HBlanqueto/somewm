/* Implementation translation unit for the dependency-free dominant-color core
 * (dominant_color.h). Single-threaded: the core uses static scratch sized for
 * bits = 5 and must never be called from more than one thread at a time. */
#define DOMINANT_COLOR_IMPLEMENTATION
#include "dominant_color.h"