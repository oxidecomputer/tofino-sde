#include "tofino/intrinsic_metadata.p4"

/* Sample P4 program */
header_type pkt_t {
    fields {
        field_a_32 : 32;
        field_b_32 : 32;
        field_c_32 : 32;
        field_d_32 : 32;

        field_e_16 : 16;
        field_f_16 : 16;
        field_g_16 : 16;
        field_h_16 : 16;

        field_i_8 : 8;
        field_j_8 : 8;
        field_k_8 : 8;
        field_l_8 : 8;

        field_m_32 : 32;
        field_n_32 : 32;

        field_o_10 : 10;
        field_p_6  : 6;
    }
}


parser start {
    return parse_pkt;
}

header pkt_t pkt;

parser parse_pkt {
    extract(pkt);
    return ingress;
}

action action_0(){
    modify_field(pkt.field_f_16, 1);
}

action do_nothing(){
    no_op();
}

action do_nothing_1(){ 
}

@pragma entries_with_ranges 64
@pragma tcam_error_detect 1
table table_0 {
    reads {
         pkt.field_o_10 : range;
         pkt.field_g_16 : exact;
         pkt.field_h_16 : exact;
    }
    actions {
        do_nothing;
        action_0;
    }
    size : 1024;
}

/* Main control flow */

control ingress {
    apply(table_0);
}


