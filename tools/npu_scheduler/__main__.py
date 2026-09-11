from __future__ import annotations

import argparse
import sys
from pathlib import Path

import uvicorn

from .app import create_app
from .config import load_config


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the TileXR NPU script scheduler")
    parser.add_argument("--config", type=Path)
    args = parser.parse_args()
    config = load_config(args.config)
    app = create_app(config)
    uvicorn.run(app, host=config.bind, port=config.port, workers=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
