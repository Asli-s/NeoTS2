#pragma once

// simrun <install> --save-fields <lot>...: every field of the lots' saved objects and scene records,
// one line per record, clip or interaction, as tab-separated name=value pairs after the kind of line.
// Lines name their lot and object, so a tool can learn which values the game's own saves always hold
// and find the fields of another save that break them.
int dumpSaveFields(const char* install, int count, char** lots);
