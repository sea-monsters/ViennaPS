# IonBeam CPU reference differential

This card records only the rank-3 `IonBeamEtching<float, 2>` CPU and fallback
contract. The paired fixture freezes one non-redeposition, fixed-energy CPU
triangle case and compares the unmodified reference tree with Mod at OMP 1/2/4/8.

It does not add an ion-energy, reflection, redeposition, or material Vulkan seam.
Auto therefore remains CPU-authoritative and Manual Vulkan remains fail-closed;
the aggregate model matrix and deployment exit stay gated independently.
