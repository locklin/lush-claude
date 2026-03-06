/* sn28_combined.c -- Combined init function for sn28
 *
 * When all sn28 .o files are linked into a single .so, mod-load
 * needs exactly one init function.  This file calls all the
 * per-module init functions so every DX function gets registered.
 */

/* Forward-declare all per-module init functions */
extern void init_codebook(void);
extern void init_adaptknn(void);
extern void init_euclid(void);
extern void init_gbp(void);
extern void init_iac(void);
extern void init_interf(void);
extern void init_miscop(void);
extern void init_network(void);
extern void init_nlf(void);
extern void init_sn2sn1(void);

/* Combined init function called by mod-load */
void init_sn28_combined(void) {
    init_sn2sn1();      /* defines globals: neurbase, synbase, weightbase */
    init_nlf();          /* NLF class + activation functions */
    init_codebook();     /* CODEBOOK class */
    init_interf();       /* neuron/synapse field accessors */
    init_miscop();       /* field operations, mode switches */
    init_network();      /* network creation/manipulation */
    init_gbp();          /* gradient-based propagation */
    init_euclid();       /* RBF/LVQ/TMAP */
    init_iac();          /* inhibitory/associative cascade */
    init_adaptknn();     /* adaptive k-NN */
}
