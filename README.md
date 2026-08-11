# RISC-V (RV64) Assembler

一个用 C++17 实现的 RISC-V 64 位汇编器练习项目，支持两遍汇编、ELF64 可重定位目标文件输出。

## 构建

```bash
g++ -std=c++17 Assembler/Assembler.cpp -o myasstest
```

## 使用

```bash
# 默认输出到 output.o
./myasstest test.s

# 用 -o 指定输出文件名 (两种顺序均可)
./myasstest -o out.o test.s
./myasstest test.s -o out.o
# 紧凑形式
./myasstest -oout.o test.s
# 也兼容旧的位置参数形式
./myasstest test.s out.o
```

使用 `readelf` 查看生成的 ELF 文件：

```bash
readelf -a out.o
```

## 当前支持的指令集

| 扩展 | 指令 |
|------|------|
| RV64I 基础整数 | add/sub/sll/slt/sltu/xor/srl/sra/or/and + W 变体, lw/ld/sw/sd, beq/bne/blt/bge/bltu/bgeu, jal/jalr, lui/auipc, slli/srli/srai + W 变体, addi/andi/ori/xori/slti/sltiu |
| 伪指令 | li, la, call, tail, mv, ret, j, jr, bgez/bltz/bgtz/blez, not, neg, negw, nop, seqz/snez |
| M 扩展 | mul/mulh/mulhsu/mulhu/div/divu/rem/remu + W 变体 (mulw/divw/divuw/remw/remuw) |
| CSR 指令 | csrrw/csrrs/csrrc + 立即数变体 (csrrwi/csrrsi/csrrci) |
| 系统指令 | ecall, ebreak, fence, fence.i |
| 数据指令 | .byte/.half/.word/.quad/.string |
| 重定位 | .rela.text, .rela.data (R_RISCV_HI20/LO12_I/LO12_S/32) |

## 项目结构

```
├── Assembler/
│   ├── Assembler.cpp          # 汇编器主程序
│   ├── write_elf64_object.h   # ELF64 写入器
│   ├── test.s                 # 测试文件 (阶段一)
│   ├── test2.s                # 测试文件 (阶段二)
│   ├── test3.s                # 测试文件 (阶段三: 重定位)
│   └── test4.s                # 测试文件 (阶段五: M扩展/CSR/系统指令)
├── for_ass.md                 # 分析与完善记录文档
└── README.md
```

> 这是一个练习项目，仍有诸多不足之处待改进。
