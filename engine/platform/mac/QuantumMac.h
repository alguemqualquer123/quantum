// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef QuantumMac_h
#define QuantumMac_h

// Entry point of the macOS backend for exported games, on every graphic backend:
// the sokol define picks the drawable, a MacViewMetal, MacViewVulkan or MacViewGL.
// The window is WindowMac and the quantum::System surface is SystemMac, both shared
// with the editor, so what is left here is the frame loop and the build settings.
class QuantumMac {
public:

    static int init(int argc, char **argv);
};

#endif /* QuantumMac_h */
