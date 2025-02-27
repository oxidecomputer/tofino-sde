/*
Copyright 2013-present Barefoot Networks, Inc. 

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

header_type data_t {
    fields {
       f1 : 32;
       f2 : 32;
       c1 : 8;
       c2 : 8;
       c3 : 8;
       c4 : 8;
    }
}

header data_t data;

parser start {
    extract(data);
    return ingress;
}
action c1_2(val1, val2) {
    modify_field(data.c1, val1);
    modify_field(data.c2, val2);
}

action c3_4(val3, val4, port) {
    modify_field(data.c3, val3);
    modify_field(data.c4, val4);
    modify_field(standard_metadata.egress_spec, port);
}

table test1 {
    reads {
        data.f1 : exact;
    }
    actions {
        c1_2;
    }
}

table test2 {
    reads {
        data.f2 : exact;
    }
    actions {
        c3_4;
    }
    size: 1024;
}

counter cnt {
    type: packets;
    direct: test1;
}

counter cnt2 {
    type: bytes;
    direct: test1;
}

counter cnt3 {
    type: packets;
    direct: test2;
}

counter cnt4 {
    type: bytes;
    direct: test2;
}

control ingress {
    apply(test1);
    apply(test2);
}
