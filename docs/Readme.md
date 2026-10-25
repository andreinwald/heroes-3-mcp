# Mistral Le Chonk plays Heroes of Might and Magic III

This repository adds an MCP server into game engine (C++) code and game harness (Typescript) that lets a Mistral model play the game.

The agent walks the adventure map, picks up resources, visits objects, fights battles (attack, wait, defend), answers dialogs, and ends the day when movement runs out.

https://github.com/user-attachments/assets/77dab1f3-a235-43d6-a211-92ae925f6e4b

# Set up and run

Prerequisites:

1. You must own Heroes of Might and Magic III and have the original game files. On [GOG](https://www.gog.com/en/game/heroes_of_might_and_magic_3_complete_edition) the Complete Edition is about 3 EUR.
2. This fork changes the engine, so you have to compile VCMI from this repository.
3. A [Mistral API key](https://console.mistral.ai/).
4. [Node.js](https://nodejs.org/) for the game agent.

## 1. Engine and game files

This project is based on [VCMI](https://vcmi.eu/), an open-source Heroes of Might and Magic III engine. It does not include the full game: you still need the original data files (`Data`, `Maps`, `Mp3`).

The easiest source is a GOG purchase. Download the Windows offline installer (`.exe` plus matching `.bin`) from their website:

![GOG offline installer](players/images/gog_offline_installer.png)

After you compile and launch VCMI, the launcher asks where those files are. Point it at the installer (or at an existing Heroes III install).

![VCMI launcher — Heroes III data files](players/images/gamefiles.png)

More info:
- [Windows](players/Installation_Windows.md)
- [macOS](players/Installation_macOS.md)
- [Linux](players/Installation_Linux.md)
- [Android](players/Installation_Android.md)
- [iOS](players/Installation_iOS.md)

## 2. Compile the engine

- [Building VCMI for Android](developers/Building_Android.md)
- [Building VCMI for iOS](developers/Building_iOS.md)
- [Building VCMI for Linux](developers/Building_Linux.md)
- [Building VCMI for macOS](developers/Building_macOS.md)
- [Building VCMI for Windows](developers/Building_Windows.md)
- [Conan](developers/Conan.md)

Start **vcmiclient**. While the game window is open, the MCP server listens on `http://127.0.0.1:47321/mcp`.

## 3. Game agent and API key

The agent lives in [`mcp_client/`](../mcp_client). Copy `.env.example` to `.env` and put your Mistral API key in `MISTRAL_API_KEY`.

```shell
cd mcp_client
cp .env.example .env
npm install
npm start
```

Optional: `npm start -- "scout north and pick up gold"` suggests the first action; after that the model keeps playing on its own.
