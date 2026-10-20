/* Shared main loop: fixed 60 Hz simulation, render once per displayed frame. */
#include "platform.h"
#include "game.h"
#include "render.h"

int main(int argc, char **argv) {
    double last, acc = 0;
    unsigned int prev = 0;
    const double step = 1.0 / 60.0;

    if (plat_init(argc, argv) != 0) return 1;
    game_init();
    last = plat_seconds();

    while (!plat_should_quit()) {
        unsigned int held = plat_read_pad();
        unsigned int pressed = held & ~prev;
        unsigned int released = prev & ~held;
        double now = plat_seconds();
        int steps = 0;
        prev = held;

        game_input(pressed, released, held);

        acc += now - last;
        last = now;
        if (acc > 0.25) acc = 0.25;           /* don't spiral after a stall */
        while (acc >= step && steps < 8) { game_step(); acc -= step; steps++; }

        plat_rumble(game_wants_rumble() ? 200 : 0);
        game_render();
        plat_present();                       /* waits for vsync on PS3 */
    }

    game_save();
    plat_shutdown();
    return 0;
}
