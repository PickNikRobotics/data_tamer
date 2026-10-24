# Changelog

## 2.0.0

- Decode ROS 2 snapshots with header timestamps, schema catalogs with per-entry
  errors, and batches for inspection; cache rejected schemas and provide a
  packaged `.foxe` extension.
- A `string` field (a `std::string`) converts to a string; a `char[]` stays an
  array of byte values.
