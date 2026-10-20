#ifndef GAME_H
#define GAME_H

void game_init(void);
void game_input(unsigned int pressed, unsigned int released, unsigned int held);
void game_step(void);     /* one fixed 1/60 s tick */
void game_render(void);
void game_save(void);
int  game_wants_rumble(void);

#endif
