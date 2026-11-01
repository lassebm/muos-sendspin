// muOS Sendspin receiver: plays Sendspin audio (Music Assistant and other servers) on a handheld.

#include "app/app.h"

int main(int argc, char** argv) {
    auto opt = app::parse_options(argc, argv);
    if (!opt) return opt.error();
    return app::run(std::move(*opt));
}
