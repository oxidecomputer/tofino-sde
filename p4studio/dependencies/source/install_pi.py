# Copyright (C) 2024 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may
# not use this file except in compliance with the License.  You may obtain
# a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations
# under the License.
#
#
# SPDX-License-Identifier: Apache-2.0

import os
from os.path import join
from pathlib import Path

from dependencies.source.source_dependency_config import SourceDependencyConfig
from utils.processes import execute
from dependencies.source.install_grpc import get_linux_bit_version

_GIT_REF_FILE = "lib/libpi.so.0.0.0.gitref"


def download_pi(config: SourceDependencyConfig) -> None:
    git_ref = config.dependency_manager().source_dependency_attributes("pi")["git_ref"]

    repo_dir = config.download_dir(ensure_exists=False)
    if not repo_dir.exists():
        execute("git clone https://github.com/p4lang/PI.git {}".format(repo_dir))
        execute("git -c advice.detachedHead=false checkout {}".format(git_ref), repo_dir)
        execute("git submodule update --init --recursive", repo_dir)
        init_patch = get_init_mod_patch(repo_dir)
        execute('git apply {}'.format(init_patch), repo_dir)


def install_pi(config: SourceDependencyConfig) -> None:
    git_ref = config.dependency_manager().source_dependency_attributes("pi")["git_ref"]

    if not config.force and is_pi_installed(config.install_dir, git_ref):
        return

    if config.os_name == 'CentOS':
        pkg_config_path = '{install_dir}/lib/pkgconfig:{install_dir}/lib:{install_dir}/lib64:{install_dir}/lib64/pkgconfig'.format(
            install_dir=config.install_dir)
    else:
        pkg_config_path = config.install_dir.as_posix() + '/lib/pkgconfig:$PKG_CONFIG_PATH'

    download_pi(config)
    build_dir = config.build_dir(copy_download_dir=True)
    execute("./autogen.sh", build_dir)

    override_envs = {
        'LD_RUN_PATH': (config.install_dir / 'lib').as_posix(),
        'PATH': '{}/bin:{}'.format(config.install_dir, os.environ['PATH']),
        'PKG_CONFIG_PATH': pkg_config_path,
        'PYTHON': 'python3'
    }

    configure_command_template = './configure ' \
                                 'LDFLAGS=-L{install_dir}/lib ' \
                                 'CPPFLAGS=-I{install_dir}/include ' \
                                 '--prefix={install_dir} ' \
                                 '--with-proto={proto} ' \
                                 '--without-bmv2 ' \
                                 '--without-internal-rpc ' \
                                 '--without-cli'

    configure_command = configure_command_template.format(
        install_dir=config.install_dir,
        proto=config.with_proto
    )
    execute(configure_command, build_dir, override_env=override_envs)

    override_env = {'LIBRARY_PATH': '{}/lib'.format(config.install_dir)}

    grpc_attrs = config.dependency_manager().source_dependency_attributes("grpc")
    grpc_version = grpc_attrs[get_linux_bit_version()]['version']

    if grpc_version == '1.60.0':
        execute("make install -j{} CXXFLAGS='-Wno-error -std=c++17'".format(config.jobs), build_dir, override_env)
    else:
        execute("make install -j{} CXXFLAGS='-Wno-error -std=c++11'".format(config.jobs), build_dir, override_env)

    with open(join(config.install_dir.as_posix(), _GIT_REF_FILE), "w") as f:
        f.write(git_ref)


def is_pi_installed(path: Path, git_ref: str) -> bool:
    try:
        with open((path / _GIT_REF_FILE).as_posix()) as f:
            return f.readline() == git_ref
    except FileNotFoundError:
        return False


def get_init_mod_patch(output_dir: Path) -> Path:
    ########################################################################
    # Do not create init file in python module.
    ########################################################################
    patch = """diff --git a/proto/Makefile.am b/proto/Makefile.am
index 94616b2..3e8eedb 100644
--- a/proto/Makefile.am
+++ b/proto/Makefile.am
@@ -225,7 +225,7 @@ if HAVE_GRPC_PY_PLUGIN
 	@touch $(builddir)/py_out/p4/__init__.py $(builddir)/py_out/p4/v1/__init__.py
 	@touch $(builddir)/py_out/p4/config/__init__.py $(builddir)/py_out/p4/config/v1/__init__.py
 	@touch $(builddir)/py_out/p4/tmp/__init__.py
-	@touch $(builddir)/py_out/google/__init__.py $(builddir)/py_out/google/rpc/__init__.py $(builddir)/py_out/gnmi/__init__.py
+	@touch $(builddir)/py_out/google/rpc/__init__.py $(builddir)/py_out/gnmi/__init__.py
 	@touch $(builddir)/py_out/p4/server/__init__.py $(builddir)/py_out/p4/server/v1/__init__.py
 endif
 	@mv -f proto_files.tmp $@
"""  # noqa

    filename = output_dir / 'patch0.patch'

    with open(filename.as_posix(), 'w') as f:
        f.write(patch)

    return filename
