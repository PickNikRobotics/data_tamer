// The decoder's NodeNext imports use .js suffixes even when bundling its TS sources.
module.exports = {
  /** @param {import('webpack').Configuration} config */
  webpack(config) {
    config.resolve = { ...config.resolve, extensionAlias: { '.js': ['.ts', '.js'] } };
    return config;
  },
};
