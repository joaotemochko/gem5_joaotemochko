# gem5 - Instruction Flow Accelerator (IFA) Implementation

This repository is a fork of the [gem5 simulator](http://www.gem5.org/) containing modifications to implement and simulate the **Instruction Flow Accelerator (IFA)** architecture (previously referred to as Single-Thread Unit/STU).

This project is developed within the scope of research activities at **Centro Universitário FEI** and the **Instituto Federal de Educação, Ciência e Tecnologia de São Paulo (IFSP)**, focusing on high-performance architecture and energy efficiency.

## 🚀 Key Features

This custom version of gem5 includes:

* **IFA Architecture:** Implementation of the *Instruction Flow Amplifier* logic, designed to optimize instruction flow and enhance single-thread performance.
* **ISA Support:** Configurations tested and validated for **RISC-V** and **ARM** architectures.
* **McPAT Integration:** Custom scripts and parsers developed to convert gem5 output statistics (`stats.txt`, `config.json`) into **McPAT** compatible XML formats, enabling accurate power and area estimation.
* **Benchmarks:** Support and configuration scripts for running the **MiBench** suite (including *basicmath* and *blowfish*) to validate the architecture.

## 🛠️ Installation and Build

The dependencies are the same as the standard gem5. To build the simulator with IFA modifications:

```bash
# Clone the repository
git clone [https://github.com/joaotemochko/gem5_joaotemochko.git](https://github.com/joaotemochko/gem5_joaotemochko.git)
cd gem5_joaotemochko

# Example: Build for RISC-V (Optimized)
scons build/RISCV/gem5.opt -j$(nproc)

# Example: Build for ARM
scons build/ARM/gem5.opt -j$(nproc)
