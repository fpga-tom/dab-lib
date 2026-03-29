#ifndef DAB_CORE_VITERBI47_PORT_H
#define DAB_CORE_VITERBI47_PORT_H

void *create_viterbi47_port(int len);
int init_viterbi47_port(void *p, int starting_state);
int update_viterbi47_blk_port(void *p, unsigned char *syms, int nbits);
int chainback_viterbi47_port(void *p, unsigned char *data, unsigned int nbits, unsigned int endstate);
void delete_viterbi47_port(void *p);

#endif
