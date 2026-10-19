// main.cpp - entry point: parses the command line and hands over to Application.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <GLFW/glfw3.h>

#include "app/Application.h"

namespace {

void GlfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description ? description : "(no description)");
}

} // namespace

int main(int argc, char** argv) {
    uilab::AppArgs args;
    std::string error;
    if (!uilab::ParseArgs(argc, argv, args, error)) {
        if (error.empty())
            std::fputs(uilab::UsageText(), stdout);   // --help
        else
            std::fprintf(stderr, "%s\n\n%s", error.c_str(), uilab::UsageText());
        return error.empty() ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    glfwSetErrorCallback(GlfwErrorCallback);

    uilab::Application app;
    if (!app.Initialize(args))
        return EXIT_FAILURE;
    return app.Run();
}
