// The boundary curtain writes no colour (its pipeline masks colour writes). Only its depth counts.

float4 main() : SV_Target {
    return float4(1, 1, 1, 1);
}
