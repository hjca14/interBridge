# include/

Reserved for public/shared headers that need to be visible outside `src/`
(e.g. if this firmware is ever split into a library consumed by other
targets). Not used yet: all current headers live next to their
implementation under `src/`, which is simpler while the project is a
single firmware binary.
