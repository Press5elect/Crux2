/*
 * Crux2 - link stub for the console's libSceAudioIn (the microphones).
 * SPDX-FileCopyrightText: 2026 Press5elect
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
 *
 * The payload SDK has no stub for libSceAudioIn, so the title could not import it, and a title
 * reaches a system module only through its own imports: the loader binds them when it starts the
 * title (PS5_Vulkan's docs/HARDWARE_FINDINGS.md). This names the three functions ps5_mic.cpp
 * calls, in the form of the SDK's own stubs (its sce_stubs/genstub.py: symbols only, no code);
 * link-title.sh links it with the soname libSceAudioIn.sprx. An import the console does not
 * bind is null at run time, and ps5_mic.cpp checks each before it calls one.
 */

asm(".global sceAudioInOpen\n"
    ".type sceAudioInOpen @function\n"
    "sceAudioInOpen:\n");

asm(".global sceAudioInInput\n"
    ".type sceAudioInInput @function\n"
    "sceAudioInInput:\n");

asm(".global sceAudioInClose\n"
    ".type sceAudioInClose @function\n"
    "sceAudioInClose:\n");
