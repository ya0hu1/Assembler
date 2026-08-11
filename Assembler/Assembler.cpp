#include <bits/stdc++.h>
#include "write_elf64_object.h"
using namespace std;

// 核心分词: 返回 (token 文本, 起始列号(1-based)) 列表
// 列号用于错误诊断时定位光标(^)位置
vector<pair<string,int>> tokenize_with_cols(const string &s_raw){
    const string &s = s_raw;
    vector<pair<string,int>> toks;
    string cur;
    int cur_col = 0;              // 当前 token 起始列 (1-based), 0 表示尚未开始
    auto push=[&](){
        if(!cur.empty()){
            toks.push_back({cur, cur_col});
            cur.clear();
            cur_col = 0;
        }
    };
    for(size_t i=0;i<s.size();){
        char c=s[i];
        int col = (int)i + 1;     // 1-based 列号

        // comments - support '#' ';' '//' as comment starts
        if(c=='#' || c==';' || (c=='/' && i+1<s.size() && s[i+1]=='/')) break;

        if(isspace((unsigned char)c)){
            push();
            i++;
            continue;
        }
        // treat comma as separator (do NOT append "," token)
        if(c==','){
            push();
            i++;
            continue;
        }
        // keep ':' '(' ')' as separate tokens (':' used for labels)
        if(c==':' ){
            push();
            toks.push_back({":", col});
            i++;
            continue;
        }
        if(c=='(' || c==')'){
            push();
            string t(1,c);
            toks.push_back({t, col});
            i++;
            continue;
        }
        // 引号内内容作为一个完整token（支持 .string "hello world"）
        if(c=='"'){
            push();                            // 先把已积累的 token 推出
            string q;
            size_t start = i;                  // 记录引号 token 起始列
            q.push_back(c);
            i++;
            while(i<s.size() && s[i] != '"'){
                q.push_back(s[i]);
                i++;
            }
            if(i<s.size()){
                q.push_back(s[i]);             // 闭合引号
                i++;
            }
            toks.push_back({q, (int)start + 1});
            continue;
        }
        // other chars part of token
        if(cur.empty()) cur_col = col;
        cur.push_back(c);
        i++;
    }
    push();
    return toks;
}

vector<string> tokenize_line(const string &s_raw){
    auto pv = tokenize_with_cols(s_raw);
    vector<string> toks;
    toks.reserve(pv.size());
    for(auto &p : pv) toks.push_back(p.first);
    return toks;
}

// 在源行中查找某 token 文本首次出现的列号 (1-based), 找不到返回 0
// 用于错误诊断时定位光标位置
int find_token_col(const string &raw, const string &tok){
    if(tok.empty()) return 0;
    for(auto &p : tokenize_with_cols(raw)){
        if(p.first == tok) return p.second;
    }
    return 0;
}

// 从异常消息中提取可能的出错 token 文本, 用于定位光标
// 例: "Unknown register: tx"         -> "tx"
//     "Immediate parse error: 'abc'" -> "abc"
//     "Undefined label: foo"         -> "foo"
// 找不到则返回空串
string extract_token_from_msg(const string &msg){
    // 优先匹配单引号 '...' 中的内容
    size_t q1 = msg.find('\'');
    if(q1 != string::npos){
        size_t q2 = msg.find('\'', q1+1);
        if(q2 != string::npos && q2 > q1+1) return msg.substr(q1+1, q2-q1-1);
    }
    // 其次取最后一个 ": " 之后的内容
    size_t pos = msg.rfind(": ");
    if(pos != string::npos) return msg.substr(pos+2);
    return "";
}


bool is_directive(const string &tok){
    return !tok.empty() && tok[0]=='.';
}

static unordered_map<string, int> abi_to_x = {
    {"zero",0}, {"ra",1}, {"sp",2}, {"gp",3}, {"tp",4},
    {"t0",5},{"t1",6},{"t2",7},
    {"s0",8},{"fp",8},{"s1",9},
    {"a0",10},{"a1",11},{"a2",12},{"a3",13},{"a4",14},{"a5",15},{"a6",16},{"a7",17},
    {"s2",18},{"s3",19},{"s4",20},{"s5",21},{"s6",22},{"s7",23},{"s8",24},{"s9",25},{"s10",26},{"s11",27},
    {"t3",28},{"t4",29},{"t5",30},{"t6",31}
};


int reg_id(const string &r){
    // ABI name
    auto it = abi_to_x.find(r);
    if(it != abi_to_x.end()){
        int v = it->second;
        // 修复 Bug-A4: 防御性检查,确保返回值在合法范围
        if(v < 0 || v > 31) throw runtime_error("Register out of range: " + r);
        return v;
    }

    // xN form
    if(r.size() >= 2 && r[0] == 'x'){
        try{
            int v=stoi(r.substr(1));
            if(v<0||v>31) throw runtime_error("Register out of range");
            return v;
        }catch(exception &e){
            throw runtime_error("Unknown register: " + r);
        }
    }

    throw runtime_error("Unknown register: " + r);
}

//string to longlong
long long parse_imm(const string &s){
    if(s.empty()) throw runtime_error("empty immediate");
    try{
        if(s.size()>2 && s[0]=='0' && (s[1]=='x' || s[1]=='X')) return stoll(s, nullptr, 16);
        return stoll(s, nullptr, 10);
    }catch(const exception &e){
        throw runtime_error(string("Immediate parse error: '") + s + "'");
    }
}

// 解析带引号的字符串内容，处理转义字符 (支持 \n \t \r \0 \\ \" \')
string parse_string_literal(const string &tok){
    if(tok.size()<2 || tok.front()!='"' || tok.back()!='"')
        throw runtime_error("Expected quoted string: "+tok);
    string raw = tok.substr(1, tok.size()-2);
    string out;
    for(size_t i=0;i<raw.size();i++){
        if(raw[i]=='\\' && i+1<raw.size()){
            char n = raw[i+1];
            switch(n){
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case '0': out.push_back('\0'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case '\'': out.push_back('\''); break;
                default: out.push_back(n); break;
            }
            i++;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

static inline uint32_t pack_r(uint32_t funct7, uint32_t rs2, uint32_t rs1, uint32_t funct3, uint32_t rd, uint32_t opcode){
    return (funct7<<25) | (rs2<<20) | (rs1<<15) | (funct3<<12) | (rd<<7) | (opcode&0x7f);
}
static inline uint32_t pack_i(int32_t imm, uint32_t rs1, uint32_t funct3, uint32_t rd, uint32_t opcode){
    uint32_t uimm = ((uint32_t)imm) & 0xfff;
    return (uimm<<20) | (rs1<<15) | (funct3<<12) | (rd<<7) | (opcode&0x7f);
}
static inline uint32_t pack_s(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t funct3, uint32_t opcode){
    uint32_t imm12 = ((uint32_t)imm) & 0xfff;
    uint32_t imm11_5 = (imm12>>5) & 0x7f;
    uint32_t imm4_0  = imm12 & 0x1f;
    return (imm11_5<<25) | (rs2<<20) | (rs1<<15) | (funct3<<12) | (imm4_0<<7) | (opcode&0x7f);
}
static inline uint32_t pack_b(int32_t imm, uint32_t rs2, uint32_t rs1, uint32_t funct3, uint32_t opcode){
    // imm is branch offset in bytes; must fit 13-bit signed with low bit zero
    uint32_t imm13 = ((uint32_t)imm) & 0x1fff;
    uint32_t imm12 = (imm13>>12) & 0x1;           // imm[12]
    uint32_t imm10_5 = (imm13>>5) & 0x3f;         // imm[10:5]
    uint32_t imm4_1 = (imm13>>1) & 0xf;           // imm[4:1]
    uint32_t imm11 = (imm13>>11) & 0x1;           // imm[11]
    return (imm12<<31) | (imm10_5<<25) | (rs2<<20) | (rs1<<15) | (funct3<<12) | (imm4_1<<8) | (imm11<<7) | (opcode&0x7f);
}
static inline uint32_t pack_u(int32_t imm, uint32_t rd, uint32_t opcode){
    uint32_t imm20 = ((uint32_t)imm) & 0xfffff000u;
    return (imm20) | (rd<<7) | (opcode&0x7f);
}
static inline uint32_t pack_j(int32_t imm, uint32_t rd, uint32_t opcode){
    // imm is signed 21-bit (imm[20:1] <<1). Pack as:
    uint32_t imm21 = ((uint32_t)imm) & 0x1fffff;
    uint32_t imm20 = (imm21>>20) & 0x1;
    uint32_t imm10_1 = (imm21>>1) & 0x3ff;
    uint32_t imm11 = (imm21>>11) & 0x1;
    uint32_t imm19_12 = (imm21>>12) & 0xff;
    return (imm20<<31) | (imm10_1<<21) | (imm11<<20) | (imm19_12<<12) | (rd<<7) | (opcode&0x7f);
}

//检查 v（带符号整数）是否能用 bits 位的带符号二补数表示
static inline bool fits_signed(long long v, int bits){
    long long lo = -(1LL<<(bits-1));
    long long hi = (1LL<<(bits-1)) - 1;
    return v>=lo && v<=hi;
}

// ========== 表驱动指令编码 ==========
// 指令格式分类
enum InstrFormat {
    FMT_R,          // add rd, rs1, rs2
    FMT_I_ARITH,    // addi rd, rs1, imm
    FMT_I_ARITH_W,  // addiw rd, rs1, imm (opcode=0x1b)
    FMT_I_SHIFT,    // slli rd, rs1, shamt (6-bit)
    FMT_I_SHIFT_W,  // slliw rd, rs1, shamt (5-bit, opcode=0x1b)
    FMT_I_LOAD,     // ld rd, imm(rs1)
    FMT_I_JALR,     // jalr rd, rs1, imm
    FMT_S,          // sd rs2, imm(rs1)
    FMT_B,          // beq rs1, rs2, label
    FMT_U,          // lui rd, imm
    FMT_J,          // jal rd, label
    FMT_I_CSR,      // csrrw rd, csr, rs1
    FMT_I_CSR_IMM,  // csrrwi rd, csr, imm5
    FMT_NONE,       // ecall/ebreak (无操作数)
};

// 指令定义：一条指令的完整编码信息
struct InstrDef {
    const char* mnemonic;
    uint32_t opcode;
    uint32_t funct3;
    uint32_t funct7;   // 移位指令用
    InstrFormat format;
};

// 指令表：覆盖 RV64I 全部基本指令 + W 变体
static const InstrDef instr_table[] = {
    // R-type (opcode=0x33)
    {"add",  0x33, 0x0, 0x00, FMT_R},  {"sub",  0x33, 0x0, 0x20, FMT_R},
    {"sll",  0x33, 0x1, 0x00, FMT_R},  {"slt",  0x33, 0x2, 0x00, FMT_R},
    {"sltu", 0x33, 0x3, 0x00, FMT_R},  {"xor",  0x33, 0x4, 0x00, FMT_R},
    {"srl",  0x33, 0x5, 0x00, FMT_R},  {"sra",  0x33, 0x5, 0x20, FMT_R},
    {"or",   0x33, 0x6, 0x00, FMT_R},  {"and",  0x33, 0x7, 0x00, FMT_R},
    // R-type W (opcode=0x3b)
    {"addw", 0x3b, 0x0, 0x00, FMT_R},  {"subw", 0x3b, 0x0, 0x20, FMT_R},
    {"sllw", 0x3b, 0x1, 0x00, FMT_R},  {"srlw", 0x3b, 0x5, 0x00, FMT_R},
    {"sraw", 0x3b, 0x5, 0x20, FMT_R},
    // I-type arith (opcode=0x13)
    {"addi",  0x13, 0x0, 0, FMT_I_ARITH},  {"slti",  0x13, 0x2, 0, FMT_I_ARITH},
    {"sltiu", 0x13, 0x3, 0, FMT_I_ARITH},  {"xori",  0x13, 0x4, 0, FMT_I_ARITH},
    {"ori",   0x13, 0x6, 0, FMT_I_ARITH},  {"andi",  0x13, 0x7, 0, FMT_I_ARITH},
    {"slli",  0x13, 0x1, 0x00, FMT_I_SHIFT},{"srli",  0x13, 0x5, 0x00, FMT_I_SHIFT},
    {"srai",  0x13, 0x5, 0x20, FMT_I_SHIFT},
    // I-type arith W (opcode=0x1b)
    {"addiw", 0x1b, 0x0, 0, FMT_I_ARITH_W}, {"slliw", 0x1b, 0x1, 0x00, FMT_I_SHIFT_W},
    {"srliw", 0x1b, 0x5, 0x00, FMT_I_SHIFT_W},{"sraiw", 0x1b, 0x5, 0x20, FMT_I_SHIFT_W},
    // I-type load (opcode=0x03)
    {"lb", 0x03, 0x0, 0, FMT_I_LOAD},  {"lh", 0x03, 0x1, 0, FMT_I_LOAD},
    {"lw", 0x03, 0x2, 0, FMT_I_LOAD},  {"ld", 0x03, 0x3, 0, FMT_I_LOAD},
    // I-type jalr (opcode=0x67)
    {"jalr", 0x67, 0x0, 0, FMT_I_JALR},
    // S-type (opcode=0x23)
    {"sb", 0x23, 0x0, 0, FMT_S},  {"sh", 0x23, 0x1, 0, FMT_S},
    {"sw", 0x23, 0x2, 0, FMT_S},  {"sd", 0x23, 0x3, 0, FMT_S},
    // B-type (opcode=0x63)
    {"beq",  0x63, 0x0, 0, FMT_B},  {"bne",  0x63, 0x1, 0, FMT_B},
    {"blt",  0x63, 0x4, 0, FMT_B},  {"bge",  0x63, 0x5, 0, FMT_B},
    {"bltu", 0x63, 0x6, 0, FMT_B},  {"bgeu", 0x63, 0x7, 0, FMT_B},
    // U-type
    {"lui",   0x37, 0, 0, FMT_U},  {"auipc", 0x17, 0, 0, FMT_U},
    // J-type
    {"jal", 0x6f, 0, 0, FMT_J},
    // M扩展 RV64 (opcode=0x33, funct7=0x01)
    {"mul",    0x33, 0x0, 0x01, FMT_R},  {"mulh",   0x33, 0x1, 0x01, FMT_R},
    {"mulhsu", 0x33, 0x2, 0x01, FMT_R},  {"mulhu",  0x33, 0x3, 0x01, FMT_R},
    {"div",    0x33, 0x4, 0x01, FMT_R},  {"divu",   0x33, 0x5, 0x01, FMT_R},
    {"rem",    0x33, 0x6, 0x01, FMT_R},  {"remu",   0x33, 0x7, 0x01, FMT_R},
    // M扩展 W变体 (opcode=0x3b, funct7=0x01)
    {"mulw",   0x3b, 0x0, 0x01, FMT_R},  {"divw",   0x3b, 0x4, 0x01, FMT_R},
    {"divuw",  0x3b, 0x5, 0x01, FMT_R},  {"remw",   0x3b, 0x6, 0x01, FMT_R},
    {"remuw",  0x3b, 0x7, 0x01, FMT_R},
    // CSR 指令 (opcode=0x73)
    {"csrrw",  0x73, 0x1, 0, FMT_I_CSR},  {"csrrs",  0x73, 0x2, 0, FMT_I_CSR},
    {"csrrc",  0x73, 0x3, 0, FMT_I_CSR},  {"csrrwi", 0x73, 0x5, 0, FMT_I_CSR_IMM},
    {"csrrsi", 0x73, 0x6, 0, FMT_I_CSR_IMM}, {"csrrci", 0x73, 0x7, 0, FMT_I_CSR_IMM},
    // 系统指令
    {"ecall",  0x73, 0x0, 0, FMT_NONE},  {"ebreak", 0x73, 0x0, 0, FMT_NONE},
};

// 查表：根据助记符查找指令定义
const InstrDef* lookup_instr(const string& op){
    for(auto& d : instr_table)
        if(op == d.mnemonic) return &d;
    return nullptr;
}

// CSR 寄存器名称 -> 地址映射
static unordered_map<string, uint32_t> csr_names = {
    {"mstatus", 0x300}, {"misa", 0x301}, {"mie", 0x304}, {"mtvec", 0x305},
    {"mscratch", 0x340}, {"mepc", 0x341}, {"mcause", 0x342}, {"mtval", 0x343},
    {"mip", 0x344}, {"sie", 0x104}, {"sip", 0x142}, {"stvec", 0x105},
    {"sscratch", 0x140}, {"sepc", 0x141}, {"scause", 0x142}, {"stval", 0x143},
    {"satp", 0x180}, {"cycle", 0xc00}, {"time", 0xc01}, {"instret", 0xc02},
};

// 解析 CSR 操作数：支持符号名(mstatus等)和数字(0x300)
uint32_t parse_csr(const string &s){
    auto it = csr_names.find(s);
    if(it != csr_names.end()) return it->second;
    return (uint32_t)parse_imm(s);
}

// 解析 fence 的 pred/succ 参数 (如 "rw", "iorw")
int parse_fence_arg(const string &s){
    int v = 0;
    for(char c : s){
        switch(tolower(c)){
            case 'i': v |= 0x8; break;
            case 'o': v |= 0x4; break;
            case 'r': v |= 0x2; break;
            case 'w': v |= 0x1; break;
        }
    }
    return v;
}

uint32_t encode_r_type(const string &op, int rd, int rs1, int rs2){
    // R-type: add, sub, sll, slt, sltu, xor, srl, sra, or, and
    //opcode=0x33
    //add rd=rs1+rs2; sub rd=rs1-rs2; sll rd=rs1<<(rs2 & mask)(rs2取低位作为移位量)
    if(op=="add") return pack_r(0x00, rs2, rs1, 0x0, rd, 0x33);
    if(op=="sub") return pack_r(0x20, rs2, rs1, 0x0, rd, 0x33);
    if(op=="sll") return pack_r(0x00, rs2, rs1, 0x1, rd, 0x33);
    //slt rd=(rs1<rs2)? 1:0(有符号比较) sltu rd=(rs1<rs2 unsigned)? 1:0
    if(op=="slt") return pack_r(0x00, rs2, rs1, 0x2, rd, 0x33);
    if(op=="sltu")return pack_r(0x00, rs2, rs1, 0x3, rd, 0x33);
    //xor rd=rs1^rs2(异或) srl rd=rs1>>(rs2 & mask)(逻辑右移高位补0)
    if(op=="xor") return pack_r(0x00, rs2, rs1, 0x4, rd, 0x33);
    if(op=="srl") return pack_r(0x00, rs2, rs1, 0x5, rd, 0x33);
    //sra 算术右移 or rd=rs1|rs2 and rd=rs1&rs2
    if(op=="sra") return pack_r(0x20, rs2, rs1, 0x5, rd, 0x33);
    if(op=="or")  return pack_r(0x00, rs2, rs1, 0x6, rd, 0x33);
    if(op=="and") return pack_r(0x00, rs2, rs1, 0x7, rd, 0x33);
    // RV64I W 变体 (32位运算，opcode=0x3b)
    // addw: rd = (rs1 + rs2)[31:0] 符号扩展
    // subw: rd = (rs1 - rs2)[31:0] 符号扩展
    if(op=="addw") return pack_r(0x00, rs2, rs1, 0x0, rd, 0x3b);
    if(op=="subw") return pack_r(0x20, rs2, rs1, 0x0, rd, 0x3b);
    if(op=="sllw") return pack_r(0x00, rs2, rs1, 0x1, rd, 0x3b);
    if(op=="srlw") return pack_r(0x00, rs2, rs1, 0x5, rd, 0x3b);
    if(op=="sraw") return pack_r(0x20, rs2, rs1, 0x5, rd, 0x3b);
    throw runtime_error("Unknown R-type: "+op);
}

uint32_t encode_i_type(const string &op, int rd, int rs1, long long imm){
    //addi rd=rs1+imm
    if(op=="addi"){
        if(!fits_signed(imm,12)) throw runtime_error("addi immediate out of range");
        return pack_i((int32_t)imm, rs1, 0x0, rd, 0x13);
    }
    //slti rd, rs1, imm 设置 rd 为 1/0，比较 rs1 与立即数（有符号/无符号）
    if(op=="slti"){
        if(!fits_signed(imm,12)) throw runtime_error("slti imm out of range");
        return pack_i((int32_t)imm, rs1, 0x2, rd, 0x13);
    }
    if(op=="sltiu"){
        if(!fits_signed(imm,12)) throw runtime_error("sltiu imm out of range");
        return pack_i((int32_t)imm, rs1, 0x3, rd, 0x13);
    }
    if(op=="xori"){
        if(!fits_signed(imm,12)) throw runtime_error("xori imm out of range");
        return pack_i((int32_t)imm, rs1, 0x4, rd, 0x13);
    }
    if(op=="ori"){
        if(!fits_signed(imm,12)) throw runtime_error("ori imm out of range");
        return pack_i((int32_t)imm, rs1, 0x6, rd, 0x13);
    }
    if(op=="andi"){
        if(!fits_signed(imm,12)) throw runtime_error("andi imm out of range");
        return pack_i((int32_t)imm, rs1, 0x7, rd, 0x13);
    }
    if(op=="slli"){
        // rd = rs1 << shamt
        if(imm < 0 || imm > 63) throw runtime_error("slli shamt out of range");
        uint32_t funct7 = 0x00;
        uint32_t funct3 = 0x1;
        uint32_t opcode = 0x13;
        uint32_t shamt = (uint32_t)imm;
        return (funct7<<25) | (shamt<<20) | (rs1<<15) | (funct3<<12) | (rd<<7) | opcode;
    }
    if(op=="srli"){
        //逻辑右移
        if(imm < 0 || imm > 63) throw runtime_error("srli shamt out of range");
        uint32_t funct7 = 0x00;
        uint32_t funct3 = 0x5;
        uint32_t opcode = 0x13;
        uint32_t shamt = (uint32_t)imm;
        return (funct7<<25) | (shamt<<20) | (rs1<<15) | (funct3<<12) | (rd<<7) | opcode;
    }
    //算术右移(保留符号)
    if(op=="srai"){
        if(imm < 0 || imm > 63) throw runtime_error("srai shamt out of range");
        uint32_t funct7 = 0x20;
        uint32_t funct3 = 0x5;
        uint32_t opcode = 0x13;
        uint32_t shamt = (uint32_t)imm;
        return (funct7<<25) | (shamt<<20) | (rs1<<15) | (funct3<<12) | (rd<<7) | opcode;
    }
    // loads
    //lb 从 rs1 + imm 读取 1 字节，有符号扩展到寄存器宽度 lh(2字节) 有符号拓展
    //lw(4字节) ld(8字节)
    if(op=="lb") { if(!fits_signed(imm,12)) throw runtime_error("lb imm out of range"); return pack_i((int32_t)imm, rs1, 0x0, rd, 0x03); }
    if(op=="lh") { if(!fits_signed(imm,12)) throw runtime_error("lh imm out of range"); return pack_i((int32_t)imm, rs1, 0x1, rd, 0x03); }
    if(op=="lw") { if(!fits_signed(imm,12)) throw runtime_error("lw imm out of range"); return pack_i((int32_t)imm, rs1, 0x2, rd, 0x03); }
    if(op=="ld") { if(!fits_signed(imm,12)) throw runtime_error("ld imm out of range"); return pack_i((int32_t)imm, rs1, 0x3, rd, 0x03); }
    // jalr rd->PC+4(返回地址) 跳转到(rs1+imm) & ~1(把最低位清0)
    if(op=="jalr"){
        if(!fits_signed(imm,12)) throw runtime_error("jalr imm out of range");
        return pack_i((int32_t)imm, rs1, 0x0, rd, 0x67);
    }
    // RV64I I-type W 变体 (opcode=0x1b)
    if(op=="addiw"){
        if(!fits_signed(imm,12)) throw runtime_error("addiw immediate out of range");
        return pack_i((int32_t)imm, rs1, 0x0, rd, 0x1b);
    }
    if(op=="slliw"){
        if(imm < 0 || imm > 31) throw runtime_error("slliw shamt out of range");
        return (0x00<<25) | ((uint32_t)imm<<20) | (rs1<<15) | (0x1<<12) | (rd<<7) | 0x1b;
    }
    if(op=="srliw"){
        if(imm < 0 || imm > 31) throw runtime_error("srliw shamt out of range");
        return (0x00<<25) | ((uint32_t)imm<<20) | (rs1<<15) | (0x5<<12) | (rd<<7) | 0x1b;
    }
    if(op=="sraiw"){
        if(imm < 0 || imm > 31) throw runtime_error("sraiw shamt out of range");
        return (0x20<<25) | ((uint32_t)imm<<20) | (rs1<<15) | (0x5<<12) | (rd<<7) | 0x1b;
    }
    throw runtime_error("Unknown I-type: "+op);
}

uint32_t encode_s_type(const string &op, int rs2, int rs1, long long imm){
    //写入1248字节
    if(op=="sb"){ if(!fits_signed(imm,12)) throw runtime_error("sb imm out of range"); return pack_s((int32_t)imm, rs2, rs1, 0x0, 0x23); }
    if(op=="sh"){ if(!fits_signed(imm,12)) throw runtime_error("sh imm out of range"); return pack_s((int32_t)imm, rs2, rs1, 0x1, 0x23); }
    if(op=="sw"){ if(!fits_signed(imm,12)) throw runtime_error("sw imm out of range"); return pack_s((int32_t)imm, rs2, rs1, 0x2, 0x23); }
    if(op=="sd"){ if(!fits_signed(imm,12)) throw runtime_error("sd imm out of range"); return pack_s((int32_t)imm, rs2, rs1, 0x3, 0x23); }
    throw runtime_error("Unknown S-type: "+op);
}

uint32_t encode_b_type(const string &op, int rs1, int rs2, long long rel){
    if(!fits_signed(rel,13)) throw runtime_error("branch offset out of range");
    if((rel & 0x1) != 0) throw runtime_error("branch offset not aligned");
    //beq rs1 rs2 label =时跳转;bne 不等;blt 有符号小于; bge有符号大于等于;
    if(op=="beq") return pack_b((int32_t)rel, rs2, rs1, 0x0, 0x63);
    if(op=="bne") return pack_b((int32_t)rel, rs2, rs1, 0x1, 0x63);
    if(op=="blt") return pack_b((int32_t)rel, rs2, rs1, 0x4, 0x63);
    if(op=="bge") return pack_b((int32_t)rel, rs2, rs1, 0x5, 0x63);
    if(op=="bltu")return pack_b((int32_t)rel, rs2, rs1, 0x6, 0x63);
    if(op=="bgeu")return pack_b((int32_t)rel, rs2, rs1, 0x7, 0x63);
    throw runtime_error("Unknown B-type: "+op);
}

uint32_t encode_u_type(const string &op, int rd, long long imm){
    //lui rd,imm 将 imm[31:12] 放进 rd 的高 20 位，rd 的低 12 位清 0。
    //等价于 rd = imm & 0xfffff000。常用于构建大常数（与 addi 配合使用）
    if(op=="lui") return pack_u((int32_t)imm, rd, 0x37);
    //rd = PC + imm（imm = high 20 bits << 12）。
    //常用于 position independent addressing（例如生成 PC 相对地址高位）
    if(op=="auipc") return pack_u((int32_t)imm, rd, 0x17);
    throw runtime_error("Unknown U-type: "+op);
}

uint32_t encode_j_type(const string &op, int rd, long long rel){
    if(!fits_signed(rel,21)) throw runtime_error("jal offset out of range");
    if((rel & 0x1) != 0) throw runtime_error("jal offset not aligned");
    //rd = PC + 4（保存返回地址），然后 PC = PC + imm 跳转（imm 是带符号 21-bit，最低位隐含 0）
    if(op=="jal") return pack_j((int32_t)rel, rd, 0x6f);
    throw runtime_error("Unknown J-type: "+op);
}

// 尝试展开伪指令
// 返回 true 表示 toks 已被展开到 out 中
static bool expand_pseudo(
    const vector<string>& toks,
    vector<vector<string>>& out)
{
    if(toks.empty()) return false;

    const string &op = toks[0];

    //  控制流 
    if(op == "j"){
        out.push_back({"jal", "x0", toks[1]});
        return true;
    }

    if(op == "jr"){
        out.push_back({"jalr", "x0", toks[1], "0"});
        return true;
    }

    if(op == "ret"){
        out.push_back({"jalr", "x0", "ra", "0"});
        return true;
    }

    //  条件分支 
    if(op == "beqz"){
        out.push_back({"beq", toks[1], "x0", toks[2]});
        return true;
    }

    if(op == "bnez"){
        out.push_back({"bne", toks[1], "x0", toks[2]});
        return true;
    }

    // ble rs1, rs2, L  ->  bge rs2, rs1, L
    if(op == "ble"){
        out.push_back({"bge", toks[2], toks[1], toks[3]});
        return true;
    }

    if(op == "bgt"){
        out.push_back({"blt", toks[2], toks[1], toks[3]});
        return true;
    }

    if(op == "bleu"){
        out.push_back({"bgeu", toks[2], toks[1], toks[3]});
        return true;
    }

    if(op == "bgtu"){
        out.push_back({"bltu", toks[2], toks[1], toks[3]});
        return true;
    }

    //  li rd, imm (Load Immediate)
    //  小立即数[-2048,2047]: addi rd, x0, imm
    //  大立即数: lui rd, hi20 + addi rd, rd, lo12
    //  hi20 = (imm + 0x800) >> 12  -- 0x800补偿低12位符号扩展的进位
    //  lo12 = imm - (hi20 << 12)   -- 保证落在[-2048,2047]
    if(op == "li"){
        if(toks.size() < 3) throw runtime_error("li: missing operands");
        long long imm = parse_imm(toks[2]);
        if(fits_signed(imm, 12)){
            // addi rd, x0, imm  -- x0作为源寄存器，等价于rd=imm
            out.push_back({"addi", toks[1], "x0", toks[2]});
        } else {
            // lui rd, hi20 ; addi rd, rd, lo12
            long long hi20 = (imm + 0x800) >> 12;
            long long lo12 = imm - (hi20 << 12);
            // pack_u期望传入左移12位后的值(imm & 0xFFFFF000)
            out.push_back({"lui", toks[1], to_string(hi20 << 12)});
            if(lo12 != 0)
                out.push_back({"addi", toks[1], toks[1], to_string(lo12)});
        }
        return true;
    }

    //  数据搬运 
    if(op == "mv"){
        out.push_back({"addi", toks[1], toks[2], "0"});
        return true;
    }

    if(op == "nop"){
        out.push_back({"addi", "x0", "x0", "0"});
        return true;
    }

    //  简单算术 / 比较 
    if(op == "neg"){
        out.push_back({"sub", toks[1], "x0", toks[2]});
        return true;
    }

    if(op == "seqz"){
        out.push_back({"sltiu", toks[1], toks[2], "1"});
        return true;
    }

    if(op == "snez"){
        out.push_back({"sltu", toks[1], "x0", toks[2]});
        return true;
    }

    // la rd, symbol -> __la_hi + __la_lo (第二遍查symtab计算hi20/lo12)
    if(op == "la"){
        if(toks.size() < 3) throw runtime_error("la: missing operands");
        out.push_back({"__la_hi", toks[1], toks[2]});
        out.push_back({"__la_lo", toks[1], toks[2]});
        return true;
    }
    // call symbol -> jal ra, symbol (简化版，范围±1MB)
    if(op == "call"){
        out.push_back({"jal", "ra", toks[1]});
        return true;
    }
    // tail symbol -> jal x0, symbol
    if(op == "tail"){
        out.push_back({"jal", "x0", toks[1]});
        return true;
    }
    // 零比较分支: bgez/bltz/bgtz/blez
    if(op == "bgez"){ out.push_back({"bge", toks[1], "x0", toks[2]}); return true; }
    if(op == "bltz"){ out.push_back({"blt", toks[1], "x0", toks[2]}); return true; }
    if(op == "bgtz"){ out.push_back({"blt", "x0", toks[1], toks[2]}); return true; }
    if(op == "blez"){ out.push_back({"bge", "x0", toks[1], toks[2]}); return true; }
    // not rd, rs -> xori rd, rs, -1
    if(op == "not"){ out.push_back({"xori", toks[1], toks[2], "-1"}); return true; }
    // negw rd, rs -> subw rd, x0, rs
    if(op == "negw"){ out.push_back({"subw", toks[1], "x0", toks[2]}); return true; }
    // jalr 多操作数格式
    if(op == "jalr"){
        if(toks.size() == 2){
            out.push_back({"jalr", "ra", toks[1], "0"});
            return true;
        }
        if(toks.size() == 3){
            out.push_back({"jalr", toks[1], toks[2], "0"});
            return true;
        }
        if(toks.size() == 6 && toks[3]=="(" && toks[5]==")"){
            out.push_back({"jalr", toks[1], toks[4], toks[2]});
            return true;
        }
        return false;
    }

    // fence: fence [pred, succ]  默认 rw,rw
    if(op == "fence"){
        if(toks.size() >= 3)
            out.push_back({"__fence", toks[1], toks[2]});
        else
            out.push_back({"__fence", "rw", "rw"});
        return true;
    }
    if(op == "fence.i"){
        out.push_back({"__fence_i"});
        return true;
    }

    return false;
}



//解析立即数偏移寻址
pair<long long,int> parse_mem_operand(const vector<string> &toks, int startIdx){
    if(startIdx+3 >= (int)toks.size()) throw runtime_error("bad memory operand");
    string imm_s = toks[startIdx];
    string lpar = toks[startIdx+1];
    string reg_s = toks[startIdx+2];
    string rpar = toks[startIdx+3];
    if(lpar != "(" || rpar != ")") throw runtime_error("bad memory operand parentheses");
    long long imm = parse_imm(imm_s);
    int base = reg_id(reg_s);
    return {imm, base};
}


int main(int argc, char**argv){
    //关闭cpp的iostream与c标准库同步,禁用自动刷新,加速i/o对于大文件处理更快,不影响解析逻辑,性能优化

    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // ===== 命令行参数解析 =====
    //   用法: prog [-o output.o] input.s
    //   也兼容旧的位置参数形式: prog input.s output.o
    //   -o <file>      指定输出文件名 (默认: output.o)
    //   -o<file>       同上, 紧凑形式
    //   -h, --help     显示帮助
    string outfile = "output.o";   // 默认输出文件名
    string infile;
    bool outfile_set = false;      // 输出文件是否已被指定 (用于检测 -o 与位置参数冲突)

    for(int i=1;i<argc;i++){
        string arg = argv[i];
        if(arg=="-o"){
            if(i+1>=argc){
                cerr<<"Error: option -o requires an argument\n";
                cerr<<"Usage: "<<argv[0]<<" [-o output.o] input.s\n";
                return 1;
            }
            outfile = argv[++i];
            outfile_set = true;
        } else if(arg.size()>2 && arg.substr(0,2)=="-o"){
            // 紧凑形式 -ooutput.o
            outfile = arg.substr(2);
            outfile_set = true;
        } else if(arg=="-h" || arg=="--help"){
            cerr<<"Usage: "<<argv[0]<<" [-o output.o] input.s\n";
            cerr<<"  -o <file>   指定输出文件名 (默认: output.o)\n";
            return 0;
        } else if(!arg.empty() && arg[0]=='-' && arg!="-"){
            cerr<<"Error: unknown option '"<<arg<<"'\n";
            cerr<<"Usage: "<<argv[0]<<" [-o output.o] input.s\n";
            return 1;
        } else {
            // 位置参数: 第一个为输入文件; 若存在第二个, 则作为输出文件 (兼容旧用法)
            if(infile.empty()){
                infile = arg;
            } else {
                if(outfile_set){
                    cerr<<"Error: output file specified twice (via -o and positionally)\n";
                    cerr<<"Usage: "<<argv[0]<<" [-o output.o] input.s\n";
                    return 1;
                }
                outfile = arg;
                outfile_set = true;
            }
        }
    }

    if(infile.empty()){
        cerr<<"Usage: "<<argv[0]<<" [-o output.o] input.s\n";
        return 1;
    }

    ifstream ifs(infile);
    if(!ifs){ cerr<<"Cannot open "<<infile<<"\n"; return 2; }

    vector<LineInfo> lines;
    string raw;
    size_t lineno=0;
    while(getline(ifs,raw)){
        lineno++;
        lines.push_back({lineno,raw});
    }

    // ===== 错误诊断子系统 =====
    // 统一错误/警告输出格式: file:line:col: error: msg  + 源码上下文 + 光标(^)
    // 收集全部错误后统一退出, 而非遇到第一个错误即终止 (修复 P10: 遇错即退出)
    int err_count = 0;
    int warn_count = 0;

    // 按行号 (1-based, 顺序连续) 取原始源行; 越界返回空串
    auto get_raw_line = [&](size_t ln) -> string {
        if(ln>=1 && ln<=lines.size()) return lines[ln-1].raw;
        return "";
    };

    // 输出一条诊断信息; kind 为 "error" 或 "warning"
    auto diagnose = [&](size_t ln, int col, const string &kind, const string &msg){
        if(kind=="error") err_count++; else warn_count++;
        cerr<<infile<<":"<<ln;
        if(col>0) cerr<<":"<<col;
        cerr<<": "<<kind<<": "<<msg<<"\n";
        string rawline = get_raw_line(ln);
        if(!rawline.empty()){
            cerr<<"  "<<rawline<<"\n";
            if(col>0) cerr<<string((size_t)col+1, ' ')<<"^\n";
        }
    };

    // 报错: 根据 msg (或显式 tok_hint) 在源行中定位列号后输出
    auto report_error = [&](size_t ln, const string &msg, const string &tok_hint=""){
        string tok = tok_hint.empty() ? extract_token_from_msg(msg) : tok_hint;
        int col = tok.empty() ? 0 : find_token_col(get_raw_line(ln), tok);
        diagnose(ln, col, "error", msg);
    };
    auto report_warning = [&](size_t ln, const string &msg, const string &tok_hint=""){
        string tok = tok_hint.empty() ? extract_token_from_msg(msg) : tok_hint;
        int col = tok.empty() ? 0 : find_token_col(get_raw_line(ln), tok);
        diagnose(ln, col, "warning", msg);
    };

    //Data structures for first pass
    SectionKind cursec=SEC_NONE;
    uint32_t text_off=0;
    uint32_t data_off=0;
    vector<Instr> instrs;
    unordered_map<string,Label> symtab;
    unordered_map<string,size_t> global_names; // .globl声明的符号名 -> 声明所在行号
    vector<string> pending_labels; // 延迟记录的标签(修复 Bug-A2)
    auto flush_labels = [&](){
        for(auto &lbl : pending_labels){
            uint32_t addr = (cursec==SEC_TEXT? text_off : (cursec==SEC_DATA? data_off : 0));
            symtab[lbl]=Label{lbl,cursec,addr};
        }
        pending_labels.clear();
    };

    for(auto&L :lines){
      try {
        auto toks=tokenize_line(L.raw);
        if(toks.empty()) continue;

        if(toks.size()>=2 && toks[1]==":"){
            string lbl=toks[0];
            pending_labels.push_back(lbl); // 延迟记录,等后续指令对齐后再写入

            vector<string> rest;
            for(size_t i=2;i<toks.size();++i) rest.push_back(toks[i]);
            if(rest.empty()) continue;
            toks=rest;
        }
        if(is_directive(toks[0])){
            string d = toks[0];
            if(d==".text"){
                flush_labels();
                cursec = SEC_TEXT;
                continue;
            } else if(d==".data"){
                flush_labels();
                cursec = SEC_DATA;
                continue;
            } else if(d==".globl" || d==".global"){
                if(toks.size()>=2) global_names[toks[1]] = L.lineno;
                continue;
            } else if(d==".align"){
                if(toks.size()>=2){
                    long long val = stoll(toks[1]);
                    if(val<0 || val>31) throw runtime_error(".align requires value in range 0-31");
                    uint32_t align = (1u<<val);
                    if(cursec==SEC_TEXT) text_off = ( (text_off + align - 1) / align ) * align;
                    if(cursec==SEC_DATA) data_off = ( (data_off + align - 1) / align ) * align;
                }
                flush_labels();
                continue;
            }else if(d==".word"||d==".4byte"){
                data_off = (data_off + 3) & ~3u; // 对齐4字节 (修复 Bug-A3)
                flush_labels();
                if(cursec!=SEC_DATA) report_warning(L.lineno, d+" directive used outside .data section", d);
                Instr ins; ins.lineno=L.lineno; ins.sec=SEC_DATA;
                ins.offset=data_off; ins.toks=toks;
                instrs.push_back(ins); data_off+=4; continue;
            }else if(d==".byte"){
                flush_labels();
                Instr ins; ins.lineno=L.lineno; ins.sec=SEC_DATA;
                ins.offset=data_off; ins.toks=toks;
                instrs.push_back(ins); data_off+=1; continue;
            }else if(d==".2byte"||d==".half"){
                data_off = (data_off + 1) & ~1u; // 对齐2字节
                flush_labels();
                Instr ins; ins.lineno=L.lineno; ins.sec=SEC_DATA;
                ins.offset=data_off; ins.toks=toks;
                instrs.push_back(ins); data_off+=2; continue;
            }else if(d==".8byte"||d==".quad"){
                data_off = (data_off + 7) & ~7u; // 对齐8字节
                flush_labels();
                Instr ins; ins.lineno=L.lineno; ins.sec=SEC_DATA;
                ins.offset=data_off; ins.toks=toks;
                instrs.push_back(ins); data_off+=8; continue;
            }else if(d==".string"||d==".asciz"||d==".ascii"){
                flush_labels();
                Instr ins; ins.lineno=L.lineno; ins.sec=SEC_DATA;
                ins.offset=data_off; ins.toks=toks;
                instrs.push_back(ins);
                string s = parse_string_literal(toks[1]);
                data_off += s.size();
                if(d==".string"||d==".asciz") data_off += 1; // \0结尾
                continue;
            }else{
                continue;
            }
        }
        if(cursec==SEC_NONE){
            report_error(L.lineno, "instruction outside any section (expected .text or .data)", toks[0]);
            continue;
        }
        flush_labels();

        vector<vector<string>> expanded;
        if(expand_pseudo(toks, expanded)){
            for(auto &et : expanded){
                Instr ins;
                ins.lineno = L.lineno;
                ins.sec = cursec;
                ins.toks = et;

                if(cursec == SEC_TEXT){
                    ins.offset = text_off;
                    instrs.push_back(ins);
                    text_off += 4;
                }else if(cursec == SEC_DATA){
                    ins.offset = data_off;
                    instrs.push_back(ins);
                    data_off += 4;
                }
            }
            continue;
        }

        // 非伪指令：直接作为单条真实指令处理
        {
            Instr ins;
            ins.lineno = L.lineno;
            ins.sec = cursec;
            ins.toks = toks;
            if(cursec == SEC_TEXT){
                ins.offset = text_off;
                instrs.push_back(ins);
                text_off += 4;
            }else if(cursec == SEC_DATA){
                ins.offset = data_off;
                instrs.push_back(ins);
                data_off += 4;
            }
        }
      } catch(exception& e){
        report_error(L.lineno, e.what());
      }
    }

    flush_labels(); // flush 残留标签
    // 标记全局符号; .globl 声明但未定义的符号给出警告
    for(auto &gv : global_names){
        auto it = symtab.find(gv.first);
        if(it != symtab.end()) it->second.is_global = true;
        else report_warning(gv.second, ".globl declares undefined symbol '"+gv.first+"'", gv.first);
    }

    // 第一遍若存在错误, 汇总后退出, 不进入第二遍 (避免级联错误)
    if(err_count>0){
        cerr<<"Assembler: "<<err_count<<" error(s) generated in first pass.\n";
        return 1;
    }

    //first pass done
    // Print first pass summary (optional)
    cout<<"=== First pass results ===\n";
    cout.flush();   // 确保 pass-1 进度信息先于 pass-2 的 cerr 诊断输出, 避免缓冲交错

    vector<RelocEntry> text_relocs, data_relocs;

    vector<uint8_t> textout(text_off,0);   // 修复 Bug-A5: 不再强制最小1字节
    vector<uint8_t> dataout(data_off,0);

    auto get_label_addr=[&](const string &Lname)->uint32_t{
        auto it=symtab.find(Lname);
        if(it==symtab.end()){
            throw runtime_error("Undefined label: "+Lname);
        }
        return it->second.addr;
    };

    for(auto &ins:instrs){
        if(ins.sec==SEC_DATA){
            if(ins.toks.empty()) continue;
            string d = ins.toks[0];
            try {
                if(ins.toks.size()<2) throw runtime_error(d+" requires an operand");
                if(d==".word"||d==".4byte"){
                    if(ins.offset + 4 > dataout.size()) throw runtime_error("data overflow");
                    bool is_imm = true; long long v = 0;
                    try { v = parse_imm(ins.toks[1]); }
                    catch(...) { is_imm = false; }
                    if(is_imm){
                        uint32_t w = (uint32_t)v;
                        memcpy(&dataout[ins.offset], &w, 4);
                    } else {
                        // 符号引用：生成 R_RISCV_32 重定位
                        uint32_t w = 0;
                        memcpy(&dataout[ins.offset], &w, 4);
                        data_relocs.push_back({ins.offset, R_RISCV_32, ins.toks[1], 0});
                    }
                } else if(d==".byte"){
                    long long v = parse_imm(ins.toks[1]);
                    dataout[ins.offset] = (uint8_t)v;
                } else if(d==".2byte"||d==".half"){
                    long long v = parse_imm(ins.toks[1]);
                    uint16_t w = (uint16_t)v;
                    if(ins.offset + 2 > dataout.size()) throw runtime_error("data overflow");
                    memcpy(&dataout[ins.offset], &w, 2);
                } else if(d==".8byte"||d==".quad"){
                    long long v = parse_imm(ins.toks[1]);
                    uint64_t w = (uint64_t)v;
                    if(ins.offset + 8 > dataout.size()) throw runtime_error("data overflow");
                    memcpy(&dataout[ins.offset], &w, 8);
                } else if(d==".string"||d==".asciz"||d==".ascii"){
                    string s = parse_string_literal(ins.toks[1]);
                    if(d==".string"||d==".asciz") s.push_back('\0');
                    if(ins.offset + s.size() > dataout.size()) throw runtime_error("data overflow");
                    memcpy(&dataout[ins.offset], s.data(), s.size());
                }
            } catch(exception &e){
                report_error(ins.lineno, e.what());
            }
            continue;
        }
        if(ins.sec==SEC_TEXT){
            if(ins.toks.empty()) continue;

            string op=ins.toks[0];
            uint32_t encoded=0;

            try{
                // 表驱动指令编码：查表 -> switch(format)
            const InstrDef *def = lookup_instr(op);
            if(def){
                switch(def->format){
                case FMT_R:
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    encoded = pack_r(def->funct7, reg_id(ins.toks[3]), reg_id(ins.toks[2]),
                                     def->funct3, reg_id(ins.toks[1]), def->opcode);
                    break;
                case FMT_I_ARITH:
                case FMT_I_ARITH_W: {
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]); int rs1=reg_id(ins.toks[2]);
                    long long imm=parse_imm(ins.toks[3]);
                    if(!fits_signed(imm,12)) throw runtime_error("immediate "+ins.toks[3]+" out of range (12-bit signed)");
                    encoded = pack_i((int32_t)imm, rs1, def->funct3, rd, def->opcode);
                    break;
                }
                case FMT_I_SHIFT: {
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]); int rs1=reg_id(ins.toks[2]);
                    long long shamt=parse_imm(ins.toks[3]);
                    if(shamt<0||shamt>63) throw runtime_error("shift amount "+ins.toks[3]+" out of range (0-63)");
                    encoded = (def->funct7<<25)|((uint32_t)shamt<<20)|(rs1<<15)|(def->funct3<<12)|(rd<<7)|def->opcode;
                    break;
                }
                case FMT_I_SHIFT_W: {
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]); int rs1=reg_id(ins.toks[2]);
                    long long shamt=parse_imm(ins.toks[3]);
                    if(shamt<0||shamt>31) throw runtime_error("shift amount "+ins.toks[3]+" out of range (0-31)");
                    encoded = (def->funct7<<25)|((uint32_t)shamt<<20)|(rs1<<15)|(def->funct3<<12)|(rd<<7)|def->opcode;
                    break;
                }
                case FMT_I_LOAD: {
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]); auto mem=parse_mem_operand(ins.toks,2);
                    if(!fits_signed(mem.first,12)) throw runtime_error("load offset "+to_string(mem.first)+" out of range (12-bit signed)");
                    encoded = pack_i((int32_t)mem.first, mem.second, def->funct3, rd, def->opcode);
                    break;
                }
                case FMT_I_JALR: {
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]); int rs1=reg_id(ins.toks[2]);
                    long long imm=parse_imm(ins.toks[3]);
                    encoded = pack_i((int32_t)imm, rs1, def->funct3, rd, def->opcode);
                    break;
                }
                case FMT_S: {
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rs2=reg_id(ins.toks[1]); auto mem=parse_mem_operand(ins.toks,2);
                    if(!fits_signed(mem.first,12)) throw runtime_error("store offset "+to_string(mem.first)+" out of range (12-bit signed)");
                    encoded = pack_s((int32_t)mem.first, rs2, mem.second, def->funct3, def->opcode);
                    break;
                }
                case FMT_B: {
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rs1=reg_id(ins.toks[1]); int rs2=reg_id(ins.toks[2]);
                    uint32_t tgt=get_label_addr(ins.toks[3]);
                    long long rel=(long long)tgt-(long long)ins.offset;
                    if(!fits_signed(rel,13)) throw runtime_error("branch target out of range (offset "+to_string(rel)+", must fit ±4KiB)");
                    encoded = pack_b((int32_t)rel, rs2, rs1, def->funct3, def->opcode);
                    break;
                }
                case FMT_U: {
                    if(ins.toks.size()<3) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]); long long imm=parse_imm(ins.toks[2]);
                    encoded = pack_u((int32_t)imm, rd, def->opcode);
                    break;
                }
                case FMT_J: {
                    if(ins.toks.size()<3) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]); uint32_t tgt=get_label_addr(ins.toks[2]);
                    long long rel=(long long)tgt-(long long)ins.offset;
                    if(!fits_signed(rel,21)) throw runtime_error("jal target out of range (offset "+to_string(rel)+", must fit ±1MiB)");
                    encoded = pack_j((int32_t)rel, rd, def->opcode);
                    break;
                }
                case FMT_I_CSR: {
                    // csrrw rd, csr, rs1
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]);
                    uint32_t csr=parse_csr(ins.toks[2]);
                    int rs1=reg_id(ins.toks[3]);
                    encoded = (csr<<20)|(rs1<<15)|(def->funct3<<12)|(rd<<7)|def->opcode;
                    break;
                }
                case FMT_I_CSR_IMM: {
                    // csrrwi rd, csr, imm5
                    if(ins.toks.size()<4) throw runtime_error("too few operands for '"+op+"'");
                    int rd=reg_id(ins.toks[1]);
                    uint32_t csr=parse_csr(ins.toks[2]);
                    long long imm=parse_imm(ins.toks[3]);
                    if(imm<0||imm>31) throw runtime_error("csr immediate "+ins.toks[3]+" out of range (0-31)");
                    encoded = (csr<<20)|((uint32_t)imm<<15)|(def->funct3<<12)|(rd<<7)|def->opcode;
                    break;
                }
                case FMT_NONE: {
                    // ecall=0x73, ebreak=0x00100073
                    encoded = (op=="ebreak") ? 0x00100073 : 0x00000073;
                    break;
                }
                }
            }
            // __la_hi rd, symbol -> lui rd, 0 + R_RISCV_HI20 (统一走重定位)
            // 修复 Bug-A1: 本地符号也必须产生重定位, 由链接器回填最终地址
            else if(op=="__la_hi"){
                if(ins.toks.size()<3) throw runtime_error("too few operands for 'la'");
                int rd = reg_id(ins.toks[1]);
                string symname = ins.toks[2];
                encoded = encode_u_type("lui", rd, 0);
                text_relocs.push_back({ins.offset, R_RISCV_HI20, symname, 0});
            }
            // __la_lo rd, symbol -> addi rd, rd, 0 + R_RISCV_LO12_I (统一走重定位)
            // 修复 Bug-A1: 本地符号也必须产生重定位, 由链接器回填最终地址
            else if(op=="__la_lo"){
                if(ins.toks.size()<3) throw runtime_error("too few operands for 'la'");
                int rd = reg_id(ins.toks[1]);
                string symname = ins.toks[2];
                encoded = encode_i_type("addi", rd, rd, 0);
                text_relocs.push_back({ins.offset, R_RISCV_LO12_I, symname, 0});
            }
            // fence pred, succ -> 编码为 0x0f 类型
            else if(op=="__fence"){
                if(ins.toks.size()<3) throw runtime_error("too few operands for 'fence'");
                int pred = parse_fence_arg(ins.toks[1]);
                int succ = parse_fence_arg(ins.toks[2]);
                encoded = ((pred<<4 | succ) << 20) | 0x0f;
            }
            // fence.i -> 0x0000100f
            else if(op=="__fence_i"){
                encoded = 0x0000100f;
            }
            else {
                throw runtime_error("Unknown opcode: "+op);
            }
            }
            catch(exception &e){
                report_error(ins.lineno, e.what());
                continue;
            }

            if(ins.offset + 4 > textout.size()){ report_error(ins.lineno, "text section buffer overflow"); continue; }
            // write little-endian
            uint32_t w = encoded;
            textout[ins.offset+0] = w & 0xff;
            textout[ins.offset+1] = (w>>8) & 0xff;
            textout[ins.offset+2] = (w>>16) & 0xff;
            textout[ins.offset+3] = (w>>24) & 0xff;
        }
    }

    // 第二遍结束: 若存在错误, 汇总后退出, 不写出 (可能不完整的) 目标文件
    if(err_count>0){
        if(warn_count>0)
            cerr<<"Assembler: "<<err_count<<" error(s), "<<warn_count<<" warning(s) generated.\n";
        else
            cerr<<"Assembler: "<<err_count<<" error(s) generated.\n";
        return 1;
    }

cout<<"=== SECOND PASS DONE ===\n";
cout<<"Generated text.bin and data.bin\n";

write_elf64_object(outfile, textout, dataout, symtab, text_relocs, data_relocs);

cout<<"Wrote ELF64 relocatable object: "<<outfile<<"\n";
    return 0;

}