# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2020-2023 Napatech A/S

if not is_linux or not dpdk_conf.has('RTE_ARCH_X86_64')
        build = false
        reason = 'only supported on x86_64 Linux'
        subdir_done()
endif

# includes
includes = [
        include_directories('.'),
        include_directories('include'),
        include_directories('ntlog'),
        include_directories('ntutil'),
        include_directories('nthw/core/include'),
        include_directories('nthw'),
        include_directories('nthw/supported'),
        include_directories('nthw/model'),
        include_directories('nthw/ntnic_meter'),
        include_directories('nthw/flow_filter'),
        include_directories('nthw/flow_api'),
        include_directories('nim/'),
]

# headers
headers = files('rte_pmd_ntnic.h')

# all sources
sources = files(
        'adapter/nt4ga_adapter.c',
        'adapter/nt4ga_stat/nt4ga_stat.c',
        'dbsconfig/ntnic_dbsconfig.c',
        'link_mgmt/link_100g/nt4ga_link_100g.c',
        'link_mgmt/link_agx_100g/nt4ga_agx_link_100g.c',
        'link_mgmt/nt4ga_link.c',
        'nim/i2c_nim.c',
        'ntnic_filter/ntnic_filter.c',
        'ntnic_xstats/ntnic_xstats.c',
        'nthw/dbs/nthw_dbs.c',
        'nthw/supported/nthw_fpga_9563_055_049_0000.c',
        'nthw/supported/nthw_fpga_9574_055_049_0000.c',
        'nthw/supported/nthw_fpga_instances.c',
        'nthw/supported/nthw_fpga_mod_str_map.c',
        'nthw/core/nt200a0x/clock_profiles/nthw_fpga_clk9563.c',
        'nthw/core/nt200a0x/nthw_fpga_nt200a0x.c',
        'nthw/core/nt400dxx/nthw_fpga_nt400dxx.c',
        'nthw/core/nt200a0x/reset/nthw_fpga_rst9563.c',
        'nthw/core/nt400dxx/reset/nthw_fpga_rst9574.c',
        'nthw/core/nt200a0x/reset/nthw_fpga_rst_nt200a0x.c',
        'nthw/core/nt400dxx/reset/nthw_fpga_rst_nt400dxx.c',
        'nthw/core/nthw_fpga.c',
        'nthw/core/nthw_gmf.c',
        'nthw/core/nthw_gfg.c',
        'nthw/core/nthw_tsm.c',
        'nthw/core/nthw_gpio_phy.c',
        'nthw/core/nthw_hif.c',
        'nthw/core/nthw_i2cm.c',
        'nthw/core/nthw_igam.c',
        'nthw/core/nthw_iic.c',
        'nthw/core/nthw_mac_pcs.c',
        'nthw/core/nthw_pcie3.c',
        'nthw/core/nthw_pca9532.c',
        'nthw/core/nthw_pcal6416a.c',
        'nthw/core/nthw_pcm_nt400dxx.c',
        'nthw/core/nthw_phy_tile.c',
        'nthw/core/nthw_prm_nt400dxx.c',
        'nthw/core/nthw_si5332_si5156.c',
        'nthw/core/nthw_rpf.c',
        'nthw/core/nthw_rmc.c',
        'nthw/core/nthw_sdc.c',
        'nthw/core/nthw_si5340.c',
        'nthw/core/nthw_spim.c',
        'nthw/core/nthw_spis.c',
        'nthw/core/nthw_spi_v3.c',
        'nthw/stat/nthw_stat.c',
        'nthw/flow_api/flow_api.c',
        'nthw/flow_api/flow_group.c',
        'nthw/flow_api/flow_id_table.c',
        'nthw/flow_api/hw_mod/hw_mod_backend.c',
        'nthw/flow_api/profile_inline/flm_age_queue.c',
        'nthw/flow_api/profile_inline/flm_lrn_queue.c',
        'nthw/flow_api/profile_inline/flm_evt_queue.c',
        'nthw/flow_api/profile_inline/flow_api_profile_inline.c',
        'nthw/flow_api/profile_inline/flow_api_hw_db_inline.c',
        'nthw/flow_api/flow_backend/flow_backend.c',
        'nthw/flow_api/flow_filter.c',
        'nthw/flow_api/flow_hasher.c',
        'nthw/flow_api/flow_hsh_cfg.c',
        'nthw/flow_api/flow_kcc.c',
        'nthw/flow_api/flow_km.c',
        'nthw/flow_api/hw_mod/hw_mod_backend.c',
        'nthw/flow_api/hw_mod/hw_mod_cat.c',
        'nthw/flow_api/hw_mod/hw_mod_flm.c',
        'nthw/flow_api/hw_mod/hw_mod_hsh.c',
        'nthw/flow_api/hw_mod/hw_mod_km.c',
        'nthw/flow_api/hw_mod/hw_mod_pdb.c',
        'nthw/flow_api/hw_mod/hw_mod_qsl.c',
        'nthw/flow_api/hw_mod/hw_mod_slc_lr.c',
        'nthw/flow_api/hw_mod/hw_mod_tpe.c',
        'nthw/flow_filter/flow_nthw_cat.c',
        'nthw/flow_filter/flow_nthw_csu.c',
        'nthw/flow_filter/flow_nthw_flm.c',
        'nthw/flow_filter/flow_nthw_hfu.c',
        'nthw/flow_filter/flow_nthw_hsh.c',
        'nthw/flow_filter/flow_nthw_ifr.c',
        'nthw/flow_filter/flow_nthw_info.c',
        'nthw/flow_filter/flow_nthw_km.c',
        'nthw/flow_filter/flow_nthw_pdb.c',
        'nthw/flow_filter/flow_nthw_qsl.c',
        'nthw/flow_filter/flow_nthw_rpp_lr.c',
        'nthw/flow_filter/flow_nthw_slc_lr.c',
        'nthw/flow_filter/flow_nthw_tx_cpy.c',
        'nthw/flow_filter/flow_nthw_tx_ins.c',
        'nthw/flow_filter/flow_nthw_tx_rpl.c',
        'nthw/ntnic_meter/ntnic_meter.c',
        'nthw/model/nthw_fpga_model.c',
        'nthw/nthw_platform.c',
        'nthw/nthw_rac.c',
        'ntlog/ntlog.c',
        'ntnic_filter/ntnic_filter.c',
        'ntutil/nt_util.c',
        'ntnic_mod_reg.c',
        'ntnic_vfio.c',
        'ntnic_ethdev.c',
)
