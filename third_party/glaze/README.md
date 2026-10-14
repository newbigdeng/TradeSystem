# Glaze

This directory contains the unmodified `include/` headers and `LICENSE` from
[Glaze v2.9.5](https://github.com/stephenberry/glaze/tree/v2.9.5).

The release is pinned because its upstream CMake target requires C++20. Glaze
v3.0.0 changes that requirement to C++23. TradeSystem keeps its existing C++20
language standard. Headers are stored in the repository so builds do not need
to download a dependency.

TradeSystem reads its startup configuration with Glaze. JSON parsing does not
run on order or market-data processing paths.
