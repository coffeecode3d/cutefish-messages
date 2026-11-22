// SPDX-FileCopyrightText: 2026 Reion Wong <reionwong@gmail.com>
// SPDX-License-Identifier: BSD-3-Clause

#include <string>

#include "messages_window.h"
#include "ui/application.h"

int main(int argc, char **argv)
{
    cutefish::ui::Application app(argc, argv);
    app.setApplicationId("org.cutefish.Messages");

    bool demo = false;
    for (const std::string &argument : app.arguments()) {
        if (argument == "--demo")
            demo = true;
    }

    cutefish::messages::MessagesWindow window(&app.loop(), demo);
    window.show();
    return app.run();
}
