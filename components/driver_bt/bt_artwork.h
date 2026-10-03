#pragma once

// look up and display the cover for a bluetooth track (online, by artist/album)
void bt_artwork_lookup(const char *artist, const char *album, const char *title);
// forget the last track and ignore pending lookups
void bt_artwork_reset(void);
