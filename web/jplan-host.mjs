import { WormholeTable } from "./wormhole-table.mjs";

const decoder = new TextDecoder("utf-8", { fatal: true });

function thrownError(value) {
  if (value instanceof Error) return value;
  const error = new Error(String(value));
  error.cause = value;
  return error;
}

export class JPlanHost {
  constructor(memory, wormholes = new WormholeTable()) {
    this.memory = memory;
    this.wormholes = wormholes;
  }

  #resolve(token, label) {
    if (!this.wormholes.has(token))
      throw new Error(`JPLAN Eval received an unknown ${label} token`);
    return this.wormholes.get(token);
  }

  #eval(environmentToken, objectTokensPtr, objectCount, sourcePtr, sourceLen) {
    const environment = this.#resolve(environmentToken, "environment");
    if (
      environment === null ||
      (typeof environment !== "object" && typeof environment !== "function")
    )
      throw new TypeError("JPLAN Eval environment must be an object or function");

    const memory = this.memory();
    const view = new DataView(memory.buffer);
    const objects = [];
    for (let i = 0; i < objectCount; i++) {
      const token = view.getBigUint64(objectTokensPtr + i * 8, true);
      objects.push(this.#resolve(token, `object[${i}]`));
    }
    const source = decoder.decode(
      new Uint8Array(memory.buffer, sourcePtr, sourceLen),
    );

    let result;
    try {
      result = new Function(
        "environment",
        "objects",
        `"use strict";\n${source}`,
      ).call(environment, environment, objects);
    } catch (error) {
      result = thrownError(error);
    }
    return this.wormholes.adopt(result);
  }

  imports() {
    return {
      ...this.wormholes.imports(),
      jplan_eval: (environmentToken, objectTokensPtr, objectCount, sourcePtr,
                   sourceLen) =>
        this.#eval(
          environmentToken,
          objectTokensPtr,
          objectCount,
          sourcePtr,
          sourceLen,
        ),
    };
  }
}
