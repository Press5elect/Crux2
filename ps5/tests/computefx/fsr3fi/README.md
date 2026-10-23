# FSR 3 frame interpolation entry points (tests only)

AMD's GLSL entry points for FSR 3's frame interpolation passes, unchanged, from the FidelityFX
SDK 1.1.4 (<https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>, tag `v1.1.4`, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`, `sdk/src/backends/vk/shaders/frameinterpolation/`),
MIT (the notice is at the top of each file; the license text is
`ps5/frontend/third_party/fidelityfx/LICENSE.txt`).

`run-computefx-tests.sh` compiles them over the vendored SDK 2.3.0 pass headers
(`ps5/frontend/third_party/fidelityfx/gpu`) and `computefx_test --fsr3-fi` runs them
(`fsr3_fi_route.h`) on Crux2's optical flow, to compare FSR 3's interpolation with Crux2's
composite. None of this goes into the title.
