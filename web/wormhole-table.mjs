export class WormholeTable {
  #next = 1n;
  #entries = new Map();

  adopt(value) {
    if (this.#next > 0xffffffffffffffffn)
      throw new Error("wormhole token space exhausted");
    const token = this.#next++;
    this.#entries.set(token, { value, refs: 1 });
    return token;
  }

  retain(token) {
    const entry = this.#entries.get(BigInt(token));
    if (entry === undefined) throw new Error("retain of unknown wormhole");
    entry.refs++;
  }

  release(token) {
    token = BigInt(token);
    const entry = this.#entries.get(token);
    if (entry === undefined) throw new Error("release of unknown wormhole");
    entry.refs--;
    if (entry.refs === 0) this.#entries.delete(token);
  }

  get(token) {
    return this.#entries.get(BigInt(token))?.value;
  }

  has(token) {
    return this.#entries.has(BigInt(token));
  }

  refCount(token) {
    return this.#entries.get(BigInt(token))?.refs ?? 0;
  }

  get size() {
    return this.#entries.size;
  }

  imports() {
    return {
      wormhole_retain: (token) => this.retain(token),
      wormhole_release: (token) => this.release(token),
    };
  }
}
