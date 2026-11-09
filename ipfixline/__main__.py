import argparse

from .sample import load_sample
from .store import Store
from .web import serve


def main():
    ap = argparse.ArgumentParser(prog="ipfixline")
    ap.add_argument("--port", type=int, default=5580)
    ap.add_argument("--db", default="ipfixline.db")
    args = ap.parse_args()
    store = Store(args.db)
    if not store.list_captures():
        load_sample(store)
    serve(store, args.port)


if __name__ == "__main__":
    main()
