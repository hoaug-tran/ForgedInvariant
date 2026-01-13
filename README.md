# ForgedInvariant ![GitHub Workflow Status](https://img.shields.io/github/actions/workflow/status/ChefKissInc/ForgedInvariant/main.yml?branch=master&logo=github&style=for-the-badge)

The plug & play kext for syncing the TSC on AMD & Intel.

> [!IMPORTANT]
> ### v1.5.1 Update: The "Hybrid" Logic (AMD Ryzen Fix)
>
> **Note:** This is a **fork** of `ForgedInvariant`, specifically modified to address AMD Ryzen wake issues.
>
> This version introduces a **Hybrid** method to fix the "Black Screen / Freeze after Wake" on AMD Ryzen. We achieved this by comparing the TSC handling logic of `Seey6/CpuTscSync` (AMD Fork) against `ForgedInvariant` and combining the best of both.
>
> **The Problem:**
> On AMD Ryzen, waking from sleep causes the base TSC registers to drift significantly between cores.
> * **Original ForgedInvariant Logic:** Only reset the `TSC_ADJUST` offset. This failed because the base registers (`MSR_TSC`) themselves were out of sync.
> * **CpuTscSync (Seey6 Fork) Logic:** Forcefully aligned the base registers but lacked the smoothness optimizations.
>
> **The Hybrid Solution:**
> 1.  **Adopted from `Seey6/CpuTscSync` (The Wake Fix):** We implemented the **Hard Sync** method. During wake, we now forcefully overwrite the **`MSR_TSC` (Register `0x10`)** on all cores. This guarantees perfect alignment and resolves the black screen/freeze.
> 2.  **Retained from `ForgedInvariant` (The Smoothness):** We kept the **Frequency Lock** feature. By asserting **`MSR_HWCR` (Register `0xC0010015`)**, we prevent TSC drift when the CPU changes P-States, ensuring the system remains stutter-free.
>
> **Result:** Reliable wake (no crashes) + buttery smooth performance.

The ForgedInvariant project is Copyright © 2024-2025 ChefKiss. The ForgedInvariant project is licensed under the `Thou Shalt Not Profit License version 1.5`. See `LICENSE`

Click [here](https://chefkiss.dev/applehax/forgedinvariant/) for more information.
