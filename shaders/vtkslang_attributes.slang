// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

module vtkslang_attributes;

[__AttributeUsage(_AttributeTargets.Var)]
public struct RangeAttribute
{
    float minimum;
    float maximum;
};

[__AttributeUsage(_AttributeTargets.Var)]
public struct LabelAttribute
{
    string text;
};
