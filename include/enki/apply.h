void enki_apply_n(enki_interpreter* i, size_t n_args);
void enki_apply(enki_interpreter* i);
size_t enki_arity(enki_value val);
void enki_enter_law(size_t arity, enki_law* law, enki_interpreter* i);