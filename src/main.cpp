
#include <unistd.h>

#include <QSurfaceFormat>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "AppWindow.h"
#include "CircusApplication.h"
#include "CircusNetwork.h"
#include "Constants.h"

int main(int argc, char** argv) {
    // Qt may take its own options out of argv: keep the command line as given, to start again with it
    std::vector<std::string> arguments(argv, argv + argc);

    int status;
    {
        spqr::CircusApplication app(argc, argv);
        app.setApplicationName(spqr::appName);

        spqr::AppWindow window(argc, argv);
        window.show();

        status = app.exec();
    }  // The window goes first: the robots' containers are removed and the shared memory cleaned

    // A restart (the web viewer's) is the same process starting over, so whoever launched Circus keeps waiting on it
    if (status == spqr::restartExitCode) {
        spqr::CircusNetwork::instance().remove();
        std::vector<char*> args;
        for (std::string& argument : arguments)
            args.push_back(argument.data());
        args.push_back(nullptr);
        execvp(args[0], args.data());
        std::cerr << "Could not start Circus again: " << std::strerror(errno) << std::endl;
        return 1;
    }
    return status;
}
