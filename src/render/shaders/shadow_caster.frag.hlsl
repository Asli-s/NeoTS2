// A Sim drawn into its shadow picture: one flat colour (the material scripts' stdMatShadowColor).
// Only the alpha, the Sim's coverage, is used later.

float4 main() : SV_Target {
    return float4(0.63, 0.63, 0.85, 1);
}
