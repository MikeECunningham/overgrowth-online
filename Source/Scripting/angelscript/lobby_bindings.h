#pragma once
class ASContext;
void AttachLobby(ASContext* context);
// Main-thread local device join/reconnect detection; no desktop input injection.
void UpdateLocalLobbyInput();
// Preserve device settings/identity before the authoritative roster compacts.
void LocalLobbyPlayerRemoving(unsigned local_index);
