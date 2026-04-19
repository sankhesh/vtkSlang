// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

module vtkslang_core;

public static const float PI = 3.14159265358979323846f;

public float reinhardTonemap(float c) { return c / (c + 1.f); }

public float gammaCorrect(float c) { return pow(max(c, 0.f), 1.f / 2.2f); }

public float3 tonemapAndGamma(float3 color)
{
    float3 tm = color / (color + float3(1.f, 1.f, 1.f));
    return float3(gammaCorrect(tm.x), gammaCorrect(tm.y), gammaCorrect(tm.z));
}
