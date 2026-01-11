# OBS Twitch Chat Plugin (prototype)

This repository contains a prototype OBS source plugin that connects to Twitch chat and renders chat lines as overlay text. Usernames are rendered with a purple outline (#8A2BE2) and white fill; messages are white only.

Features (first iteration)
- Connect to Twitch chat (IRC over WSS/TLS planned)
- Parse username and message text
- Queue latest messages (default max 10)
- Render username with purple outline + white fill; message with white fill
- Basic OBS source settings: channel, token, font, font size, max messages

Build (Windows, prototype)
1. Install OBS Studio development headers (Obs SDK / dev files) and required libs (FreeType recommended).
2. Optionally use vcpkg to install dependencies (websocketpp, asio, openssl, freetype).
3. Configure CMake:
   ```bash
   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg]/scripts/buildsystems/vcpkg.cmake
   cmake --build build --config Release
   ```
4. Drop the resulting plugin (`obs-twitch-chat.dll`) into OBS "obs-plugins" folder.

Notes
- This is a scaffold and prototype. The chat client is implemented as a stub that can be replaced with a WSS client (websocketpp/Asio) in a later step.
- See `src/` for the plugin skeleton and chat client scaffolding.

License: MIT

