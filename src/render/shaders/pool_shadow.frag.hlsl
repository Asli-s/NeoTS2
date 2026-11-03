// The pools' shadow map: each texel keeps the depth along the sun of the nearest thing over it (0 at
// the sun's end of the view, 1 at the far end).

float4 main(float4 position : SV_Position) : SV_Target {
    return float4(position.z, 0, 0, 1);
}
