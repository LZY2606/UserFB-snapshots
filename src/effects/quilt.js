const quilt = require("../quilt.js");

module.exports = function (tgen) {
  // public standalone synthesis entry point
  tgen.quilt = quilt.synthesize;

  // image quilting uses its own local LCG, so it never consumes the
  // generator seed sequence; doEffect() skips the generic seed handling
  tgen.noSeedEffects = tgen.noSeedEffects || {};
  tgen.noSeedEffects.quilt = true;

  // image quilting effect: the synthesized pixels replace the current
  // texture without any blend
  tgen.effect("quilt", {}, function ($g, params) {
    const source = params.source;
    const options = params.options || {};

    const result = quilt.synthesize(source, {
      ...options,
      width: $g.texture.width,
      height: $g.texture.height,
    });

    // write the result straight into the current texture, no blending
    const textureData = $g.texture.data;

    for (let i = 0; i < result.data.length; i++) {
      textureData[i] = result.data[i];
    }

    // keep the stored params JSON replayable: TypedArray source data is
    // stored as a plain array; synthesis results and cut state are not
    params.source = {
      width: source.width,
      height: source.height,
      data: Array.from(source.data),
    };

    params.options = { ...options };

    return params;
  });
};
