#!/usr/bin/env python

import os, sys, math, re, collections, buildstack, gnuplot, getopt, pprint, sniper_lib, sniper_config, sniper_stats
import math

#ISSUE_WIDTH = 4
#ALU_per_core = 6

# chip power
DRAM_POWER_STATIC = .102 + .009 # act_stby + ref
DRAM_POWER_READ = .388 + .271 + .019 # act + rd + dq
DRAM_POWER_WRITE = .388 + .238 + .019 + .180 # act + wr + dq + termW
DRAM_CLOCK = 266 # MHz

# interface power
DRAM_POWER_STATIC_OFFCHIP_INTERFACE = .175/8 # 175mW for the DIMM, assuming 8 chips/DIMM as below
DRAM_POWER_STATIC_TSV_INTERFACE = .00934/8 # 9.34mW for 64-bit data bus
# Re dynamic power: DDR3/SSTL dynamic power (0.019, dq above in RD and WR) is rather low (static is huge though)
# A TSV, with CMOS signaling, has almost no static, but dynamic will be higher. Let's assume it's also 0.019.

DRAM_CHIPS_PER_DIMM = 8
DRAM_DIMMS_PER_SOCKET = 4

# Round up to the next nearest power of 2 if the number isn't already a power of two
# Inspiration from http://stackoverflow.com/questions/53161/find-the-highest-order-bit-in-c
power2up_warnings = {}
def power2up(num):
  orig_num = num
  if not num:
    return 0;
  hob = 1
  num = num >> 1
  while num:
    num = num >> 1
    hob = hob << 1
  if hob != orig_num:
    hob = hob << 1
    if orig_num not in power2up_warnings:
      print '[mcpat.py] Warning: McPAT requires inputs that are powers of two for some parameters. Increasing %d to %d.' % (orig_num, hob)
      power2up_warnings[orig_num] = True
  return hob

def compute_dram_power(nread, nwrite, t, config):
  num_dram_controllers = int(config['perf_model/dram/num_controllers'])
  if num_dram_controllers > 0:
    sockets = num_dram_controllers
  else:
    sockets = math.ceil(float(config['general/total_cores']) / float(config['perf_model/dram/controllers_interleaving']))
  chips_per_dimm = float(config.get('perf_model/dram/chips_per_dimm', DRAM_CHIPS_PER_DIMM))
  dimms_per_socket = float(config.get('perf_model/dram/dimms_per_controller', DRAM_DIMMS_PER_SOCKET))

  if config.get('power/3d_dram', 'false').lower() in ('1', 'true'):
    is3d = True
  else:
    is3d = False

  ncycles = t * DRAM_CLOCK * 1e6
  read_dc = nread / sockets / (ncycles or 1)
  write_dc = nwrite / sockets / (ncycles or 1)
  power_chip_dyn  = read_dc * DRAM_POWER_READ + write_dc * DRAM_POWER_WRITE
  if is3d:
    power_chip_stat = DRAM_POWER_STATIC + DRAM_POWER_STATIC_TSV_INTERFACE
  else:
    power_chip_stat = DRAM_POWER_STATIC + DRAM_POWER_STATIC_OFFCHIP_INTERFACE
  power_socket_dyn  = power_chip_dyn * chips_per_dimm * dimms_per_socket
  power_socket_stat = power_chip_stat * chips_per_dimm * dimms_per_socket
  return (power_socket_dyn * sockets, power_socket_stat * sockets)


def dram_power(results, config):
  return compute_dram_power(
    sum(results['dram.reads']),
    sum(results['dram.writes']),
  	#Z: For microbench
    results['performance_model.elapsed_time'][0] * 1e-15,
    #results['global.time'] * 1e-15,
    config
  )


def mcpat_path():
  return os.path.join(os.path.dirname(__file__), '../mcpat/')

def mcpat_bin():
  import platform
  mcpatdir = mcpat_path()
  if platform.architecture()[0] != '64bit':
    suffix = '.32'
  elif True: # Disable if you don't want the McPAT/CACTI cache
    suffix = '.cache'
  else:
    suffix = ''
  bin = os.path.join(mcpatdir, 'mcpat-1.0%s' % suffix)
  bin = '/x/shaizeen/projs/pic/energy_analysis/latest_mcpat/mcpat/mcpat'
  if os.path.exists(bin):
    # Fancy McPAT versions haven't been downloaded yet, use the plain old one
    return bin
  else:
    os.path.join(mcpatdir, 'mcpat-1.0')

def mcpat_run(inputfile, outputfile):
  os.system("LD_LIBRARY_PATH=$LD_LIBRARY_PATH:%s %s -print_level 5 -opt_for_clk 1 -infile %s > %s" % \
    (mcpat_path(), mcpat_bin(), inputfile, outputfile))


all_items = [
  [ 'core',     .01,    [
    [ 'core',     .01,    'core' ],
    [ 'ifetch',   .01,    'core-ifetch' ],
    [ 'alu',      .01,    'core-alu-complex' ],
    [ 'int',      .01,    'core-alu-int' ],
    [ 'fp',       .01,    'core-alu-fp' ],
    [ 'mem',      .01,    'core-mem' ],
    [ 'other',    .01,    'core-other' ],
  ] ],
  [ 'icache',   .01,    'icache' ],
  [ 'dcache',   .01,    'dcache' ],
  [ 'dcache-ic',   .01,    'dcache-ic' ],
  [ 'l2',       .01,    'l2' ],
  [ 'l2-ic',       .01,    'l2-ic' ],
  [ 'l3',       .01,    'l3' ],
  [ 'nuca',     .01,    'nuca' ],
  [ 'nuca-ic',     .01,    'nuca-ic' ],
  [ 'noc',      .01,    'noc' ],
  [ 'dram',     .01,    'dram' ],
]

all_names = buildstack.get_names(all_items)

def readnlines(f, n):
  lines = ''
  for x in range(0, n):
    lines = lines + f.readline()
  return lines

def get_all_names():
  return all_names

copy_as_is = ['fs_to_cycles', 'fs_to_cycles_cores','corefreq','ipc','walltime','ncores','roi.ipstotal','roi.instrs','roi.walltime','time.walltime', 'power.NOC','power.Processor', 'power.NUCA', 'power.DRAM', 'power.Core']
assert_is_zero = [ 'global.time_begin', 'performance_model.elapsed_time_begin', 'barrier.global_time_begin']

def merge_markers (jobid, resultsdir, partial, first_marker):
  last_marker = 5000
  marker_begin = 'marker-1-X'
  marker_end 	= 'marker-2-X'
  cnt = 0
  final_results = {}
  for i in range(first_marker, last_marker):
    start_roi = marker_begin.replace("X", str(i))
    end_roi = marker_end.replace("X", str(i))
    marker_partial = [start_roi,end_roi]
    print marker_partial
    marker_results = {}
    marker_results = sniper_lib.get_results(jobid, resultsdir, partial = marker_partial)
    for name in marker_results:
     if name == 'results':
      if name not in final_results:
       final_results[name] = {}
      for key in marker_results[name]:	#merge results
       if (key in copy_as_is):
         #print 'copying', key, marker_results[name][key]
         final_results[name][key] = marker_results[name][key]
       elif (key in assert_is_zero):
         #print 'zero value', key, marker_results[name][key]
         final_results[name][key] = marker_results[name][key]
       else: #Needs summing
         if key in final_results[name]:
          if type(marker_results[name][key]) is list:
            #print 'Add list value', key, marker_results[name][key], final_results[name][key]
            final_results[name][key] = [x + y for x, y in zip(final_results[name][key], marker_results[name][key])]
            #print 'After Add', final_results[name][key]
          else:
            #print 'Add simple value', key, marker_results[name][key], final_results[name][key]
            final_results[name][key] = final_results[name][key] +  marker_results[name][key]
            #print 'After Add', final_results[name][key]
         else:
          #print 'Add first', key, marker_results[name][key]
          final_results[name][key] = marker_results[name][key]
       #print key, 'corresponds to', marker_results[name][key]
     else:
      final_results[name] =  marker_results[name]
    cnt = cnt + 1
  print 'Total data points' , cnt
  return final_results

def main(jobid, resultsdir, outputfile, powertype = 'dynamic', config = None, no_graph = False, partial = None, print_stack = True, return_data = False, bank_map_policy = 'all_ways', no_mcpat_run = False, in_cache_frac = -1.0, add_implicit_accesses = False, first_marker = 64):
  tempfile = outputfile + '.xml'

  #We need merged results for all markers we are interested in
  #results = sniper_lib.get_results(jobid, resultsdir, partial = partial)
  results = merge_markers(jobid, resultsdir, partial = partial, first_marker = first_marker)
  if config:
    results['config'] = sniper_config.parse_config(file(config).read(), results['config'])
  stats = sniper_stats.SniperStats(resultsdir = resultsdir, jobid = jobid)
  power, nuca_at_level = edit_XML(stats, results['results'], results['config'])
  power = map(lambda v: v[0], power)
  file(tempfile, "w").write('\n'.join(power))

  # Run McPAT
  if no_mcpat_run == False:
    mcpat_run(tempfile, outputfile + '.txt')

  # Parse output
  power_txt = file(outputfile + '.txt')
  power_dat = {}

  #Pick first few lines corresponding to Core 0
  components = power_txt.read().split('*'*89)[2:-1]
  #components = readnlines(power_txt, 400).split('*'*89)[2:-1]
  for component in components:
    lines = component.strip().split('\n')
    componentname = lines[0].strip().strip(':')
    values = {}
    prefix = []; spaces = []
    for line in lines[1:]:
      if not line.strip():
        continue
      elif '=' in line:
        res = re.match(' *([^=]+)= *([-+0-9.e]+)(nan)?', line)
        if res:
          name = ('/'.join(prefix + [res.group(1)])).strip()
          if res.groups()[-1] == 'nan':
            # Result is -nan. Happens for instance with 'Subthreshold Leakage with power gating'
            # on components with 0 area, such as the Instruction Scheduler for in-order cores
            value = 0.
          else:
            try:
              value = float(res.group(2))
            except:
              print >> sys.stderr, 'Invalid float:', line, res.groups()
              raise
          values[name] = value
      else:
        res = re.match('^( *)([^:(]*)', line)
        if res:
          j = len(res.group(1))
          while(spaces and j <= spaces[-1]):
            spaces = spaces[:-1]
            prefix = prefix[:-1]
          spaces.append(j)
          name = res.group(2).strip()
          prefix.append(name)
    if componentname in ('Core', 'L2', 'L3'):
      # Translate whatever level we used for NUCA back into NUCA
      if componentname == 'L%d' % nuca_at_level:
        outputname = 'NUCA'
      else:
        outputname = componentname
      if outputname not in power_dat:
        power_dat[outputname] = []
      power_dat[outputname].append(values)
    else:
      assert componentname not in power_dat
      power_dat[componentname] = values

  if not power_dat:
    raise ValueError('No valid McPAT output found')

  # Add DRAM power
  dram_dyn, dram_stat = dram_power(results['results'], results['config'])
  power_dat['DRAM'] = {
    'Peak Dynamic': dram_dyn,
    'Runtime Dynamic': dram_dyn,
    'Subthreshold Leakage': dram_stat,
    'Subthreshold Leakage with power gating': dram_stat,
    'Gate Leakage': 0,
    'Area': 0,
  }
  # Write back
  file(outputfile + '.py', 'w').write("power = " + pprint.pformat(power_dat))


  # Build stack
  ncores = int(results['config']['general/total_cores'])
  #time0_begin = results['results']['global.time_begin']
  #time0_end = results['results']['global.time_end']
  #Z: For microbench
  #time0_begin = results['results']['performance_model.elapsed_time_begin'][0]
  #time0_end = results['results']['performance_model.elapsed_time_end'][0]
  seconds = (results['results']['performance_model.elapsed_time'][0])/1e15
  results = power_stack(power_dat, powertype)
  # Plot stack
  plot_labels = []
  plot_data = {}
  if powertype == 'area':
    if print_stack:
      print '                         Area    Area %'
    for core, (res, total, other, scale) in results.items():
      plot_data[core] = {}
      total_core = 0.; total_cache = 0.
      for name, value in res:
        if print_stack:
          print '  %-12s    %6.4f mm^2   %6.4f%%' % (name, float(value), 100 * float(value) / total)
        if name.startswith('core'):
          total_core += float(value)
        elif name in ('icache', 'dcache', 'l2', 'l3', 'nuca'):
          total_cache += float(value)
        plot_labels.append(name)
        plot_data[core][name] = float(value)
      if print_stack:
        print
        print '  %-12s    %6.4f mm^2   %6.4f%%' % ('core', float(total_core), 100 * float(total_core) / total)
        print '  %-12s    %6.4f mm^2   %6.4f%%' % ('cache', float(total_cache), 100 * float(total_cache) / total)
        print '  %-12s    %6.4f mm^2   %6.4f%%' % ('total', float(total), 100 * float(total) / total)
  else:
    if print_stack:
      print '                     Power     Energy    Energy %'
    for core, (res, total, other, scale) in results.items():
      plot_data[core] = {}
      total_core = 0.; total_cache = 0.
      for name, value in res:
        if print_stack:
          energy, energy_scale = sniper_lib.scale_sci(float(value) * seconds)
          print '  %-12s    %6.4f W   %6.4f %sJ    %6.4f%%' % (name, float(value), energy, energy_scale, 100 * float(value) / total)
        if name.startswith('core'):
          total_core += float(value)
        elif name in ('icache', 'dcache', 'dcache-ic', 'l2', 'l2-ic', 'l3', 'nuca', 'nuca-ic'):
          total_cache += float(value)
        plot_labels.append(name)
        plot_data[core][name] = float(value) * seconds
      if print_stack:
        print
        energy, energy_scale = sniper_lib.scale_sci(float(total_core) * seconds)
        print '  %-12s    %6.4f W   %6.4f %sJ    %6.4f%%' % ('core', float(total_core), energy, energy_scale, 100 * float(total_core) / total)
        energy, energy_scale = sniper_lib.scale_sci(float(total_cache) * seconds)
        print '  %-12s    %6.4f W   %6.4f %sJ    %6.4f%%' % ('cache', float(total_cache), energy, energy_scale, 100 * float(total_cache) / total)
        energy, energy_scale = sniper_lib.scale_sci(float(total) * seconds)
        print '  %-12s    %6.4f W   %6.4f %sJ    %6.4f%%' % ('total', float(total), energy, energy_scale, 100 * float(total) / total)

  if not no_graph:
    # Use Gnuplot to make a stacked bargraphs of these cpi-stacks
    if 'other' in plot_labels:
      all_names.append('other')
    all_names_with_colors = zip(all_names, range(1,len(all_names)+1))
    plot_labels_with_color = [n for n in all_names_with_colors if n[0] in plot_labels]
    gnuplot.make_stacked_bargraph(outputfile, plot_labels_with_color, plot_data, 'Energy (J)')

  if return_data:
    return {'labels': plot_labels, 'power_data': plot_data, 'ncores': ncores, 'time_s': seconds}



def power_stack(power_dat, powertype = 'total', nocollapse = False):
  def getpower(powers, key = None, ic_only = False):
    def getcomponent(suffix):
      if key: return powers.get(key+'/'+suffix, 0)
      else: return powers.get(suffix, 0)
    if ic_only == True:
      if (powertype == 'total' or powertype == 'dynamic'):
			  return getcomponent('Runtime IC-total Dynamic')
      else:
        return 0
    if powertype == 'dynamic':
      return getcomponent('Runtime Dynamic')
    elif powertype == 'static':
      return getcomponent('Subthreshold Leakage with power gating') + getcomponent('Gate Leakage')
    elif powertype == 'total':
      return getcomponent('Runtime Dynamic') + getcomponent('Subthreshold Leakage with power gating') + getcomponent('Gate Leakage')
    elif powertype == 'peak':
      return getcomponent('Peak Dynamic') + getcomponent('Subthreshold Leakage with power gating') + getcomponent('Gate Leakage')
    elif powertype == 'peakdynamic':
      return getcomponent('Peak Dynamic')
    elif powertype == 'area':
      return getcomponent('Area') + getcomponent('Area Overhead')
    elif powertype == 'pic':
      return getcomponent('Runtime PIC Dynamic')
    else:
      raise ValueError('Unknown powertype %s' % powertype)
  data = {
    'l2':               sum([ getpower(power_dat['Core'][0], 'L2')]), # private L2
    'l2-ic':               sum([ getpower(power_dat['Core'][0], 'L2', True)]), # private L2
    'l3':               sum([ getpower(cache) for cache in power_dat.get('L3', []) ]),
    'nuca':             sum([ getpower(cache) for cache in power_dat.get('NUCA', []) ]),
    'nuca-ic':             sum([ getpower(cache, None, True) for cache in power_dat.get('NUCA', []) ]),
    'noc':              getpower(power_dat['Processor'], 'Total NoCs'),
    'dram':             getpower(power_dat['DRAM']),
    'core':             sum([ getpower(power_dat['Core'][0], 'Execution Unit/Instruction Scheduler')
                              + getpower(power_dat['Core'][0], 'Execution Unit/Register Files')
                              + getpower(power_dat['Core'][0], 'Execution Unit/Results Broadcast Bus')
                              + getpower(power_dat['Core'][0], 'Renaming Unit')]),
    'core-ifetch':      sum([ getpower(power_dat['Core'][0], 'Instruction Fetch Unit/Branch Predictor')
                              + getpower(power_dat['Core'][0], 'Instruction Fetch Unit/Branch Target Buffer')
                              + getpower(power_dat['Core'][0], 'Instruction Fetch Unit/Instruction Buffer')
                              + getpower(power_dat['Core'][0], 'Instruction Fetch Unit/Instruction Decoder')]),
    'icache':           sum([ getpower(power_dat['Core'][0], 'Instruction Fetch Unit/Instruction Cache')]),
    'dcache':           sum([ getpower(power_dat['Core'][0], 'Load Store Unit/Data Cache')]),
    'dcache-ic':        sum([ getpower(power_dat['Core'][0], 'Load Store Unit/Data Cache', True)]),
    'core-alu-complex': sum([ getpower(power_dat['Core'][0], 'Execution Unit/Complex ALUs')]),
    'core-alu-fp':      sum([ getpower(power_dat['Core'][0], 'Execution Unit/Floating Point Units')]),
    'core-alu-int':     sum([ getpower(power_dat['Core'][0], 'Execution Unit/Integer ALUs')]),
    'core-mem':         sum([ getpower(power_dat['Core'][0], 'Load Store Unit/LoadQ')
                              + getpower(power_dat['Core'][0], 'Load Store Unit/StoreQ')
                              + getpower(power_dat['Core'][0], 'Memory Management Unit')]),
  }
 
  #Remove Interconnect components
  data['dcache'] = data['dcache'] - data['dcache-ic']
  data['l2'] = data['l2'] - data['l2-ic']
  data['nuca'] = data['nuca'] - data['nuca-ic']

  #print getpower(power_dat['Core'][0], '')
  #print sum(data.values())
  #print (data['nuca-ic'] + data['nuca'] + data['noc'] + data['dram'])
  data['core-other'] = getpower(power_dat['Core'][0], '') - (sum(data.values()) 
																					- (data['nuca-ic'] + data['nuca'] + data['noc'] + data['dram']))
  if data['core-other'] < 0:
    data['core-other'] = 0
  return buildstack.merge_items({ 0: data }, all_items, nocollapse = nocollapse)


def edit_XML(statsobj, stats, cfg):
  #param = res['param']         #do it separately

  ncores = int(cfg['general/total_cores'])
  technology_node = int(cfg.get('power/technology_node', 45))

  l3_cacheSharedCores = long(sniper_config.get_config_default(cfg, 'perf_model/l3_cache/shared_cores', 0))
  l2_cacheSharedCores = long(sniper_config.get_config_default(cfg, 'perf_model/l2_cache/shared_cores', 0))
  nuca_at_level = False
  private_l2s = True

  if long(sniper_config.get_config_default(cfg, 'perf_model/l2_cache/data_access_time', 0)) > 0:
    num_l2s = int(math.ceil(ncores / float(l2_cacheSharedCores)))
    private_l2s = int(sniper_config.get_config(cfg, 'perf_model/l2_cache/shared_cores')) == 1
  else:
    # L2 with zero access latency can be used when we don't really want an L2, but need one to interface with the NoC
    num_l2s = 0
  if int(cfg['perf_model/cache/levels']) >= 3:
    num_l3s = int(math.ceil(ncores / float(l3_cacheSharedCores)))
    if cfg.get('perf_model/nuca/enabled') == 'true':
      print >> sys.stderr, "L3 configured, NUCA power will be ignored"
  elif cfg.get('perf_model/nuca/enabled') == 'true':
    if cfg['perf_model/dram_directory/locations'] == 'interleaved':
      nuca_cacheSharedCores = int(cfg['perf_model/dram_directory/interleaving'])
      num_nucas = int(math.ceil(ncores / float(nuca_cacheSharedCores)))
    else:
      nuca_locations = [ lid for name, lid, mid in statsobj.get_topology() if name == 'nuca-cache' ]
      # Right now we only support NUCA slices at regular interleaving
      num_nucas = len(nuca_locations)
      nuca_cacheSharedCores = ncores / num_nucas
      nuca_locations_assumed = [ i*nuca_cacheSharedCores for i in range(num_nucas) ]
      if nuca_locations != nuca_locations_assumed:
        raise ValueError('Unsupported tag directory locations %s' % cfg['perf_model/dram_directory/locations'])
    if num_l2s == 0:
      # No L2s, use them for NUCA
      num_l2s = num_nucas
      l2_cacheSharedCores = nuca_cacheSharedCores
      l = 2
      num_l3s = 0
      nuca_at_level = 2
    else:
      # We do have L2s, use L3 for NUCA
      num_l3s = num_nucas
      l3_cacheSharedCores = nuca_cacheSharedCores
      l = 3
      nuca_at_level = 3
    # Copy over NUCA statistics into L2/L3 statistics so we don't have to change anything below here
    cfg['perf_model/l%d_cache/data_access_time'%l] = cfg['perf_model/nuca/data_access_time']
    cfg['perf_model/l%d_cache/associativity'%l] = cfg['perf_model/nuca/associativity']
    cfg['perf_model/l%d_cache/cache_block_size'%l] = cfg['perf_model/l2_cache/cache_block_size']
    cfg['perf_model/l%d_cache/cache_size'%l] = cfg['perf_model/nuca/cache_size']
    cfg['perf_model/l%d_cache/writeback_time'%l] = 0
    cfg['perf_model/l%d_cache/dvfs_domain'%l] = 'global'
    stats['L%d.loads'%l] = stats['nuca-cache.reads']
    if add_implicit_accesses == True:
     stats['L%d.loads'%l] += stats['nuca-cache.dirty_evicts'] #Z Fix: Dirty evictions will cause a read
    stats['L%d.stores'%l] = stats['nuca-cache.writes']
    stats['L%d.load-misses'%l] = stats['nuca-cache.read-misses']
    stats['L%d.store-misses'%l] = stats['nuca-cache.write-misses']
    if 'nuca-cache.pic_ops_copy' in stats:
      stats['L%d.pic_ops_copy'%l] = stats['nuca-cache.pic_ops_copy']
      #stats['L%d.pic_ops_cmp'%l] = stats['nuca-cache.pic_ops_cmp']
      stats['L%d.pic_ops_cmp'%l] = [a+b for a,b in zip(stats['nuca-cache.pic_ops_cmp'], stats['nuca-cache.pic_ops_search'])]
      stats['L%d.pic_key_writes'%l] = stats['nuca-cache.pic_key_writes']
      if in_cache_frac != -1:
        stats['L%d.pic_ops_in_bank_copy_'%l + bank_map_policy] 	= [(x * in_cache_frac)  for x in stats['L%d.pic_ops_copy'%l]]
        stats['L%d.pic_ops_in_bank_cmp_'%l + bank_map_policy] 	= [(x * in_cache_frac)  for x in stats['L%d.pic_ops_cmp'%l]]
      else:
        stats['L%d.pic_ops_in_bank_copy_'%l + bank_map_policy] = stats['L%d.pic_ops_copy'%l]
        #TODO: needs fixing
        stats['L%d.pic_ops_in_bank_cmp_'%l + bank_map_policy] = stats['L%d.pic_ops_cmp'%l]
        #stats['L%d.pic_ops_in_bank_cmp_'%l + bank_map_policy] = [a+b for a,b in zip(stats['L%d.pic_ops_cmp'%l], stats['L%d.pic_ops_search'%l])]
        #stats['L%d.pic_ops_in_bank_copy_'%l + bank_map_policy] = stats['nuca-cache.pic_ops_in_bank_copy_' + bank_map_policy]
        #stats['L%d.pic_ops_in_bank_cmp_'%l + bank_map_policy] = stats['nuca-cache.pic_ops_in_bank_cmp_' + bank_map_policy]
  else:
    num_l3s = 0

  #print stats
#---------------------------
  #loads = long(stats['L1-D.loads'])
  #stores = long(stats['L1-D.stores'])
#---------------------------

  cycles_scale = stats['fs_to_cycles_cores']
  instrs = stats['performance_model.instruction_count']
  times = stats['performance_model.elapsed_time']
  cycles = map(lambda c, t: c * t, cycles_scale[:ncores], times[:ncores])
  max_system_cycles = float(max(cycles)) or 1 # avoid division by zero
  data = [ {} for core in range(ncores) ]
  for core in range(ncores):
    # Don't use performance_model.idle_elapsed_time, as it may not be up-to-date, or contain time from previous intervals, when using short intervals
    data[core]['idle_cycles'] = cycles_scale[core] * max(0, stats['global.time'] - stats['performance_model.elapsed_time'][core])
    data[core]['FP_instructions'] = (stats.get('interval_timer.uop_fp_addsub', stats.get('rob_timer.uop_fp_addsub', []))[core] \
                                  + stats.get('interval_timer.uop_fp_muldiv', stats.get('rob_timer.uop_fp_muldiv', []))[core])
    data[core]['Branch_instructions'] = (stats.get('interval_timer.uop_branch', stats.get('rob_timer.uop_branch', []))[core])
    data[core]['ialu_accesses'] = (stats.get('interval_timer.uop_load', stats.get('rob_timer.uop_load', []))[core]) \
                                + (stats.get('interval_timer.uop_store', stats.get('rob_timer.uop_store', []))[core]) \
                                + (stats.get('interval_timer.uop_generic', stats.get('rob_timer.uop_generic', []))[core])
  total_system_instructions = sum(instrs)
  DRAM_reads = int(stats['dram.reads'][0])
  DRAM_writes = int(stats['dram.writes'][0])
  #branch_misprediction = stats['branch_predictor.num-incorrect'][1]

  template=readTemplate(ncores, num_l2s, private_l2s, num_l3s, technology_node)
  #for j in range(ncores):
  for i in xrange(len(template)-1):
    #for j in range(ncores):
      if template[i][1] and template[i][1][1] in ('stat', 'cfg', 'comb'):
        core = template[i][1][2]
      else:
        core = None
      clock_core = float(sniper_config.get_config(cfg, 'perf_model/core/frequency', core))*1000
      clock_global = float(sniper_config.get_config(cfg, 'perf_model/core/frequency'))*1000
      if 'power/vdd' in cfg:
        vdd_global = float(sniper_config.get_config(cfg, 'power/vdd'))
        vdd_core = float(sniper_config.get_config(cfg, 'power/vdd', core))
      else:
        vdd_global = 0
        vdd_core = 0
      def get_clock(component):
        domain = sniper_config.get_config(cfg, component+'/dvfs_domain', core)
        if domain == 'core':
          return clock_core
        elif domain == 'global':
          return clock_global
        else:
          raise ValueError('Unknown DVFS domain %s' % domain)
      def get_vdd(component):
        domain = sniper_config.get_config(cfg, component+'/dvfs_domain', core)
        if domain == 'core':
          return vdd_core
        elif domain == 'global':
          return vdd_global
        else:
          raise ValueError('Unknown DVFS domain %s' % domain)
      issue_width = long(sniper_config.get_config(cfg, 'perf_model/core/interval_timer/dispatch_width', core))
      peak_issue_width = long(long(sniper_config.get_config(cfg, 'perf_model/core/interval_timer/dispatch_width', core)) * 1.5)
      ALU_per_core = peak_issue_width
      window_size = int(sniper_config.get_config(cfg, "perf_model/core/interval_timer/window_size", core))
      if sniper_config.get_config(cfg, "perf_model/core/type", core) == 'rob' and sniper_config.get_config_bool(cfg, "perf_model/core/rob_timer/in_order", core):
        machineType = 1 # in-order
      else:
        machineType = 0 # OoO
      latency_bp = long(sniper_config.get_config(cfg, 'perf_model/branch_predictor/mispredict_penalty', core))
        #---FROM CONFIG----------
      latency_l1_d = long(sniper_config.get_config(cfg, 'perf_model/l1_dcache/data_access_time', core))
      latency_l1_i = long(sniper_config.get_config(cfg, 'perf_model/l1_icache/data_access_time', core))
      latency_l2 = long(sniper_config.get_config_default(cfg, 'perf_model/l2_cache/data_access_time', 0, core))
      latency_l3 = long(sniper_config.get_config_default(cfg, 'perf_model/l3_cache/data_access_time', 0, core))
      l1_dcacheAssociativity = power2up(long(sniper_config.get_config_default(cfg, 'perf_model/l1_dcache/associativity', 0, core)))
      l1_dcacheBlockSize = long(sniper_config.get_config_default(cfg, 'perf_model/l1_dcache/cache_block_size', 0, core))
      l1_dcacheSize= long(sniper_config.get_config_default(cfg, 'perf_model/l1_dcache/cache_size', 0, core))
      l1_dcacheSharedCores = long(sniper_config.get_config_default(cfg, 'perf_model/l1_dcache/shared_cores', 0, core))
      l1_dcacheWriteBackTime = long(sniper_config.get_config_default(cfg, 'perf_model/l1_dcache/writeback_time', 0, core))
      l1_icacheAssociativity = power2up(long(sniper_config.get_config_default(cfg, 'perf_model/l1_icache/associativity', 0, core)))
      l1_icacheBlockSize = long(sniper_config.get_config_default(cfg, 'perf_model/l1_icache/cache_block_size', 0, core))
      l1_icacheSize = long(sniper_config.get_config_default(cfg, 'perf_model/l1_icache/cache_size', 0, core))
      l1_icacheSharedCores = long(sniper_config.get_config_default(cfg, 'perf_model/l1_icache/shared_cores', 0, core))
      l1_icacheWriteBackTime = long(sniper_config.get_config_default(cfg, 'perf_model/l1_icache/writeback_time', 0, core))
      l2_cacheAssociativity = power2up(long(sniper_config.get_config_default(cfg, 'perf_model/l2_cache/associativity', 0, core)))
      l2_cacheSize = long(sniper_config.get_config_default(cfg, 'perf_model/l2_cache/cache_size', 0, core))
      l2_cacheBlockSize = long(sniper_config.get_config_default(cfg, 'perf_model/l2_cache/cache_block_size', 0, core))
      l2_cacheWriteBackTime = long(sniper_config.get_config_default(cfg, 'perf_model/l2_cache/writeback_time', 0, core))
      l3_cacheAssociativity = power2up(long(sniper_config.get_config_default(cfg, 'perf_model/l3_cache/associativity', 0, core)))
      l3_cacheBlockSize = long(sniper_config.get_config_default(cfg, 'perf_model/l3_cache/cache_block_size', 0, core))
      l3_cacheSize = long(sniper_config.get_config_default(cfg, 'perf_model/l3_cache/cache_size', 0, core))
      l3_cacheWriteBackTime = long(sniper_config.get_config_default(cfg, 'perf_model/l3_cache/writeback_time', 0, core))

      if template[i][1]:
           #if template[i][1][1]=="cfg":
          #template[i][0] = template[i][0] % int(calc_param(template[i][1][0],config))
        if len(template[i][1]) == 1:
          # hardcoded
          template[i][0] = template[i][0] % template[i][1][0]
        elif template[i][1][1]=="cfg":
          if template[i][1][0]=="core_clock":
            template[i][0] = template[i][0] % clock_core
          elif template[i][1][0]=="core_vdd":
            template[i][0] = template[i][0] % vdd_core
          elif template[i][1][0]=="issue_width":
            template[i][0] = template[i][0] % issue_width
          elif template[i][1][0]=="peak_issue_width":
            template[i][0] = template[i][0] % peak_issue_width
          elif template[i][1][0]=="ALU_per_core":
            template[i][0] = template[i][0] % ALU_per_core
          elif template[i][1][0]=="window_size":
            template[i][0] = template[i][0] % window_size
          elif template[i][1][0]=="machineType":
            template[i][0] = template[i][0] % machineType
          elif template[i][1][0]=="L2_clock":
            template[i][0] = template[i][0] % get_clock('perf_model/l2_cache')
          elif template[i][1][0]=="L3_clock":
            template[i][0] = template[i][0] % get_clock('perf_model/l3_cache')
          elif template[i][1][0]=="NoC_clock":
            template[i][0] = template[i][0] % clock_global
          elif template[i][1][0]=="L2_vdd":
            template[i][0] = template[i][0] % get_vdd('perf_model/l2_cache')
          elif template[i][1][0]=="L3_vdd":
            template[i][0] = template[i][0] % get_vdd('perf_model/l3_cache')
          elif template[i][1][0]=="NoC_vdd":
            template[i][0] = template[i][0] % vdd_global
          else:
            raise ValueError('Unknown cfg template %s' % template[i][1][0])
        elif template[i][1][1]=="stat":
          cores_l2s = range(l2_cacheSharedCores*core, min(ncores, l2_cacheSharedCores*core+l2_cacheSharedCores))
          cores_l3s = range(l3_cacheSharedCores*core, min(ncores, l3_cacheSharedCores*core+l3_cacheSharedCores))
          # core statistics
          if template[i][1][0]=="total_cycles":
            template[i][0] = template[i][0] % cycles[core]
          elif template[i][1][0]=="busy_cycles":
            template[i][0] = template[i][0] % (cycles[core] - data[core]['idle_cycles'])
          elif template[i][1][0]=="idle_cycles":
            template[i][0] = template[i][0] % data[core]['idle_cycles']
          elif template[i][1][0]=="total_system_cycles":
            template[i][0] = template[i][0] % int(max_system_cycles)
          elif template[i][1][0]=="total_system_idle_cycles":
            template[i][0] = template[i][0] % int(0)
          elif template[i][1][0]=="total_system_busy_cycles":
            template[i][0] = template[i][0] % int(max_system_cycles)
          elif template[i][1][0]=="function_calls":
            template[i][0] = template[i][0] % int(instrs[core] * 0.05)
          elif template[i][1][0]=="IFU.duty_cycle":
            if float(((instrs[core]))/max_system_cycles) > 1:
              template[i][0] = template[i][0] % 1
            else:
              template[i][0] = template[i][0] % float(((instrs[core]))/max_system_cycles)
          elif template[i][1][0]=="LSU.duty_cycle":
            if float((long(stats['L1-D.loads'][core])+long(stats['L1-D.stores'][core]))/max_system_cycles) <= 1:
              template[i][0] = template[i][0] % float((long(stats['L1-D.loads'][core])+long(stats['L1-D.stores'][core]))/max_system_cycles)
            else:
              template[i][0] = template[i][0] % 1
          elif template[i][1][0]=="MemManU.I.duty_cycle":
            if float((long(stats['L1-I.loads'][core])+long(stats['L1-I.stores'][core]))/max_system_cycles) <= 1:
              template[i][0] = template[i][0] % float((long(stats['L1-I.loads'][core])+long(stats['L1-I.stores'][core]))/max_system_cycles)
            else:
              template[i][0] = template[i][0] % 1
          elif template[i][1][0]=="MemManU.D.duty_cycle":
            if float((long(stats['L1-D.loads'][core])+long(stats['L1-D.stores'][core]))/max_system_cycles) <= 1:
              template[i][0] = template[i][0] % float((long(stats['L1-D.loads'][core])+long(stats['L1-D.stores'][core]))/max_system_cycles)
            else:
              template[i][0] = template[i][0] % 1
          elif template[i][1][0]=="FPU.duty_cycle":
            template[i][0] = template[i][0] % min(1,float((int(data[core]['FP_instructions']))/max_system_cycles))
          elif template[i][1][0]=="MUL.duty_cycle":
            template[i][0] = template[i][0] % min(1,float((stats.get('interval_timer.uop_fp_muldiv', stats.get('rob_timer.uop_fp_muldiv', []))[core])/max_system_cycles))
          elif template[i][1][0]=="ALU.duty_cycle":             #check whether it is per FP  basis or total
            template[i][0] = template[i][0] % min(1,((instrs[core] - data[core]['FP_instructions'])/(max_system_cycles *  ALU_per_core)))
          elif template[i][1][0]=="memory.reads":
            template[i][0] = template[i][0] % DRAM_reads
          elif template[i][1][0]=="memory.writes":
            template[i][0] = template[i][0] % DRAM_writes
          elif template[i][1][0]=="memory.accesses":
            template[i][0] = template[i][0] % (int(DRAM_reads) + int(DRAM_writes))
          elif template[i][1][0]=="NoC.type":
            if 'network.shmem-1.mesh.link-in.num-requests' in stats or 'network.shmem-1.mesh.packets-in' in stats:
              # 1 = NoC
              template[i][0] = template[i][0] % 1
            else:
              # 0 = bus
              template[i][0] = template[i][0] % 0
          elif template[i][1][0]=="NoC.total_accesses":
            if 'network.shmem-1.mesh.link-in.num-requests' in stats:
              DIRECTIONS = ('up', 'down', 'left', 'right')
              #Z:Fix
              template[i][0] = template[i][0] % sum([ sum(stats['network.shmem-1.mesh.link-%s.num-requests' % direction]) for direction in DIRECTIONS ])
              #template[i][0] = template[i][0] % sum(stats['network.shmem-1.mesh.link-in.num-requests'])
            elif 'network.shmem-1.mesh.packets-in' in stats:
              template[i][0] = template[i][0] % sum(stats['network.shmem-1.mesh.packets-in'])
            elif 'network.shmem-1.bus.num-requests' in stats:
              template[i][0] = template[i][0] % int(stats['network.shmem-1.bus.num-requests'][0])  #assumption
            elif 'network.shmem-1.bus.num-packets' in stats:
              template[i][0] = template[i][0] % int(stats['network.shmem-1.bus.num-packets'][0])  #assumption
            else:
              template[i][0] = template[i][0] % int(stats['bus.num-requests'][0])  #assumption
          elif template[i][1][0]=="NoC.duty_cycle":
            if 'network.shmem-1.mesh.link-left.total-time-used' in stats:
              DIRECTIONS = ('up', 'down', 'left', 'right')
              total_time_used = sum([ sum(stats['network.shmem-1.mesh.link-%s.total-time-used' % direction]) for direction in DIRECTIONS ])
              num_links_used = sum([ sum([ v > 0 and 1 or 0 for v in stats['network.shmem-1.mesh.link-%s.num-requests' % direction] ]) for direction in DIRECTIONS ])
              # Not all links (e.g. boundary of mesh) are actually present in hardware
              # Here we assume that all real links are used at least ones
              avg_time_used = total_time_used / float(num_links_used or 1.)
  						#Z: For microbench
              #duty_cycle = avg_time_used / (stats['global.time'] or 1.)
              duty_cycle = avg_time_used / (stats['performance_model.elapsed_time_end'] [0] or 1.)
              template[i][0] = template[i][0] % duty_cycle
            elif 'network.shmem-1.mesh.packets-in' in stats:
              # Mesh network model without proper accounting. Take a wild guess...
              template[i][0] = template[i][0] % .5
            elif 'network.shmem-1.bus.time-used' in stats:
              template[i][0] = template[i][0] % min(1, cycles_scale[core]*float(stats['network.shmem-1.bus.time-used'][0])/max_system_cycles)
            else:
              template[i][0] = template[i][0] % min(1, cycles_scale[core]*float(stats['bus.time-used'][0])/max_system_cycles)
          elif template[i][1][0]=="loads":
            template[i][0] = template[i][0] % (long(stats['L1-D.loads'][core]) + long('L1-D.pic_vops_cmp' in stats and stats['L1-D.pic_vops_cmp'][core] or 0) + long('L1-D.pic_vops_search' in stats and stats['L1-D.pic_vops_search'][core] or 0))
          elif template[i][1][0]=="stores":
            template[i][0] = template[i][0] % (long(stats['L1-D.stores'][core]) + long('L1-D.pic_vops_copy' in stats and stats['L1-D.pic_vops_copy'][core] or 0))
          elif template[i][1][0]=="total_instructions":
            template[i][0] = template[i][0] % instrs[core]
          elif template[i][1][0]=="integer_ins":
            template[i][0] = template[i][0] % int(int(instrs[core]) - int(data[core]['FP_instructions']) - int(data[core]['Branch_instructions']))
          elif template[i][1][0]=="fp_ins":
            template[i][0] = template[i][0] % int(data[core]['FP_instructions'])
          elif template[i][1][0]=="itlb_total_accesses":
            template[i][0] = template[i][0] % int(instrs[core]*0.5)
          elif template[i][1][0]=="itlb_misses":
            template[i][0] = template[i][0] % int(instrs[core]*(0.5*0.5/10000))
          elif template[i][1][0]=="BTB.read_accesses":
            template[i][0] = template[i][0] % int(data[core]['Branch_instructions'])  #instrs[core]
          elif template[i][1][0]=="RAT_rename.reads":
            template[i][0] = template[i][0] % int(2 * instrs[core])
          elif template[i][1][0]=="RAT_rename.writes":
            template[i][0] = template[i][0] % int(instrs[core])
          elif template[i][1][0]=="RAT_fp_rename.reads":
            template[i][0] = template[i][0] % int(2 * int(data[core]['FP_instructions']))
          elif template[i][1][0]=="RAT_fp_rename.writes":
            template[i][0] = template[i][0] % int(data[core]['FP_instructions'])
          elif template[i][1][0]=="instr.reads":                #inst window stats
            template[i][0] = template[i][0] % instrs[core]
          elif template[i][1][0]=="instr.writes":
            template[i][0] = template[i][0] % instrs[core]
          elif template[i][1][0]=="instr.wakeup":
            template[i][0] = template[i][0] % int(instrs[core]*2)
          elif template[i][1][0]=="instr.fp.reads":
            template[i][0] = template[i][0] % int(int(data[core]['FP_instructions'])*0.5)
            #template[i][0] = template[i][0] % int(instrs[core]*0.5)		#Z fix
          elif template[i][1][0]=="instr.fp.writes":
            template[i][0] = template[i][0] % int(int(data[core]['FP_instructions'])*0.5)
            #template[i][0] = template[i][0] % int(instrs[core]*0.5)			#Z fix
          elif template[i][1][0]=="instr.fp.wakeup":
            template[i][0] = template[i][0] % int(int(data[core]['FP_instructions'])*0.5)
            #template[i][0] = template[i][0] % instrs[core]								#Z fix
          elif template[i][1][0]=="window_switches.ialu_accesses":
            template[i][0] = template[i][0] % int(data[core]['ialu_accesses'])
          elif template[i][1][0]=="window_switches.fpu_accesses":
            template[i][0] = template[i][0] % int(data[core]['FP_instructions'])
          elif template[i][1][0]=="window_switches.mul_accesses":
            template[i][0] = template[i][0] % int(stats.get('interval_timer.uop_fp_muldiv', stats.get('rob_timer.uop_fp_muldiv', []))[core])
          elif template[i][1][0]=="window_switches.cdb_alu_accesses":
            template[i][0] = template[i][0] % int(data[core]['ialu_accesses'])
          elif template[i][1][0]=="window_switches.cdb_fpu_accesses":
            template[i][0] = template[i][0] % int(data[core]['FP_instructions'])
          elif template[i][1][0]=="window_switches.cdb_mul_accesses":
            template[i][0] = template[i][0] % int(stats.get('interval_timer.uop_fp_muldiv', stats.get('rob_timer.uop_fp_muldiv', []))[core])
          elif template[i][1][0]=="RF_accesses.int_regfile_reads":
            template[i][0] = template[i][0] % int(instrs[core]*1.5)
          elif template[i][1][0]=="RF_accesses.fp_regfile_reads":
            template[i][0] = template[i][0] % int(int(data[core]['FP_instructions'])*0.25)
            #template[i][0] = template[i][0] % int(instrs[core]*0.25)	#Z fix
          elif template[i][1][0]=="RF_accesses.int_regfile_writes":
            template[i][0] = template[i][0] % int(instrs[core]*0.75)
          elif template[i][1][0]=="RF_accesses.fp_regfile_writes":
            template[i][0] = template[i][0] % int(int(data[core]['FP_instructions'])*0.125)
            #template[i][0] = template[i][0] % int(instrs[core]*0.125)	#Z fix
          elif template[i][1][0]=="ROB_reads":
            template[i][0] = template[i][0] % int(instrs[core]*1)
          elif template[i][1][0]=="ROB_writes":
            template[i][0] = template[i][0] % int(instrs[core]*1)
          elif template[i][1][0]=="branch_ins":
            template[i][0] = template[i][0] % int(data[core]['Branch_instructions'])
          elif template[i][1][0]=="branch_mis":
            template[i][0] = template[i][0] % int('branch_predictor.num-incorrect' in stats and stats['branch_predictor.num-incorrect'][core] or 0)
          elif template[i][1][0]=="committed_ins":
            template[i][0] = template[i][0] % int(instrs[core])
          elif template[i][1][0]=="committed_int":
            template[i][0] = template[i][0] % int((instrs[core])*0.5)
          elif template[i][1][0]=="committed_fp":
            template[i][0] = template[i][0] % int((instrs[core])*0.5)
          elif template[i][1][0]=="itlb.total_accesses":        #itlb equals icache reads and writes
            template[i][0] = template[i][0] % int(stats['L1-I.loads'][core] + stats['L1-I.stores'][core])
          elif template[i][1][0]=="itlb.total_misses":
            template[i][0] = template[i][0] % int(stats['L1-I.load-misses'][core] + stats['L1-I.store-misses'][core])
          elif template[i][1][0]=="icache.read_accesses":
            template[i][0] = template[i][0] % int(stats['L1-I.loads'][core])
          elif template[i][1][0]=="icache.read_misses":
            template[i][0] = template[i][0] % int(stats['L1-I.load-misses'][core])
          elif template[i][1][0]=="dtlb.total_accesses":        #dtlb equals dcache reads and writes
            template[i][0] = template[i][0] % int(stats['dtlb.access'][core])
            #template[i][0] = template[i][0] % int(stats['L1-D.loads'][core] + stats['L1-D.stores'][core])	#Z Fix
          elif template[i][1][0]=="dtlb.total_misses":
            template[i][0] = template[i][0] % int(stats['dtlb.miss'][core])
            #template[i][0] = template[i][0] % int(stats['L1-D.load-misses'][core] + stats['L1-D.store-misses'][core])	#Z fix
          elif template[i][1][0]=="dcache.read_accesses":
						#Z Fix: Dirty backinvalidation & dirty evictions will cause a read
            if add_implicit_accesses == True:
             template[i][0] = template[i][0] % (int(stats['L1-D.loads'][core]) +  int(stats['L1-D.dirty_evicts'][core]) + int(stats['L1-D.dirty_backinval'][core]))
            else:
             template[i][0] = template[i][0] % int(stats['L1-D.loads'][core])
          elif template[i][1][0]=="dcache.read_misses":
            template[i][0] = template[i][0] % int(stats['L1-D.load-misses'][core])
          elif template[i][1][0]=="dcache.write_accesses":
						#Z Fix: Read misses will cause writes later
            if add_implicit_accesses == True:
             template[i][0] = template[i][0] % ( int(stats['L1-D.stores'][core]) + int(stats['L1-D.load-misses'][core]))
            else:
             template[i][0] = template[i][0] % int(stats['L1-D.stores'][core])
          elif template[i][1][0]=="dcache.write_misses":
            template[i][0] = template[i][0] % int(stats['L1-D.store-misses'][core])
          elif template[i][1][0]=="dcache.pic_copy_ops":
            template[i][0] = template[i][0] % long('L1-D.pic_ops_copy' in stats and stats['L1-D.pic_ops_copy'][core] or 0)
          elif template[i][1][0]=="dcache.pic_copy_ops_in_bank":
            if in_cache_frac != -1:
             template[i][0] = template[i][0] % long('L1-D.pic_ops_copy' in stats and (in_cache_frac * stats['L1-D.pic_ops_copy'][core]) or 0)
            else:
             template[i][0] = template[i][0] % long('L1-D.pic_ops_in_bank_copy_' + bank_map_policy in stats and (stats['L1-D.pic_ops_in_bank_copy_' + bank_map_policy][core]) or 0)
          elif template[i][1][0]=="dcache.pic_copy_tag_access":
            template[i][0] = template[i][0] % long('L1-D.pic_ops_tag_access_copy' in stats and stats['L1-D.pic_ops_tag_access_copy'][core] or 0)
          elif template[i][1][0]=="dcache.pic_copy_read_miss":
            template[i][0] = template[i][0] % long('L1-D.pic_ops_read_miss_copy' in stats and stats['L1-D.pic_ops_read_miss_copy'][core] or 0)
          elif template[i][1][0]=="dcache.pic_copy_write_miss":
            template[i][0] = template[i][0] % long('L1-D.pic_ops_write_miss_copy' in stats and stats['L1-D.pic_ops_write_miss_copy'][core] or 0)
          elif template[i][1][0]=="dcache.pic_cmp_ops":
            template[i][0] = template[i][0] % long(('L1-D.pic_ops_cmp' in stats and stats['L1-D.pic_ops_cmp'][core] or 0) + 
																										('L1-D.pic_ops_search' in stats and stats['L1-D.pic_ops_search'][core] or 0))
          elif template[i][1][0]=="dcache.pic_cmp_ops_in_bank":
            if in_cache_frac != -1:
             template[i][0] = template[i][0] % long('L1-D.pic_ops_cmp' in stats and ((in_cache_frac * stats['L1-D.pic_ops_cmp'][core]) + (in_cache_frac * stats['L1-D.pic_ops_search'][core])) or 0)
            else:
             template[i][0] = template[i][0] % long(\
									('L1-D.pic_ops_in_bank_cmp_' + bank_map_policy in stats and (stats['L1-D.pic_ops_in_bank_cmp_' + bank_map_policy][core]) or 0)\
								+	('L1-D.pic_ops_in_bank_search_' + bank_map_policy in stats and (stats['L1-D.pic_ops_in_bank_search_' + bank_map_policy][core]) or 0))
          elif template[i][1][0]=="dcache.pic_cmp_tag_access":
            template[i][0] = template[i][0] % long(\
									('L1-D.pic_ops_tag_access_cmp' in stats and stats['L1-D.pic_ops_tag_access_cmp'][core] or 0)\
									+('L1-D.pic_ops_tag_access_search' in stats and stats['L1-D.pic_ops_tag_access_search'][core] or 0))
          elif template[i][1][0]=="dcache.pic_cmp_read_miss":
            template[i][0] = template[i][0] % long(\
									('L1-D.pic_ops_read_miss_cmp' in stats and stats['L1-D.pic_ops_read_miss_cmp'][core] or 0)\
									+('L1-D.pic_ops_read_miss_search' in stats and stats['L1-D.pic_ops_read_miss_search'][core] or 0))
          elif template[i][1][0]=="dcache.pic_cmp_write_miss":
            template[i][0] = template[i][0] % long(\
									('L1-D.pic_ops_write_miss_cmp' in stats and stats['L1-D.pic_ops_write_miss_cmp'][core] or 0)\
									+('L1-D.pic_ops_write_miss_search' in stats and stats['L1-D.pic_ops_write_miss_search'][core] or 0))
          elif template[i][1][0]=="dcache.pic_key_writes":
            #if in_cache_frac == 0:
            # template[i][0] = template[i][0] % 0
            #else:
            template[i][0] = template[i][0] % long('L1-D.pic_key_writes' in stats and stats['L1-D.pic_key_writes'][core] or 0)
          elif template[i][1][0]=="function_calls":
            template[i][0] = template[i][0] % int(instrs[core] * 0.35000)
          # L1 directory (not modeled)
          elif template[i][1][0]=="L1_directory.read_accesses":
            template[i][0] = template[i][0] % int(instrs[core] * 2)
          elif template[i][1][0]=="L1_directory.write_accesses":
            template[i][0] = template[i][0] % int(instrs[core] * 0.06667)
          elif template[i][1][0]=="L1_directory.read_misses":
            template[i][0] = template[i][0] % int(instrs[core] * 0.00408)
          elif template[i][1][0]=="L1_directory.write_misses":
            template[i][0] = template[i][0] % int(instrs[core] * 0.00005)
          elif template[i][1][0]=="L1_directory.conflicts":
            template[i][0] = template[i][0] % int(instrs[core]*0.00005)
          # L2 directory (not modeled)
          elif template[i][1][0]=="L2_directory.read_accesses":         #check for L2 or L2dir
            template[i][0] = template[i][0] % int((instrs[core])*(0.125))
          elif template[i][1][0]=="L2_directory.write_accesses":
            template[i][0] = template[i][0] % int((instrs[core])*(0.0625))
          elif template[i][1][0]=="L2_directory.read_misses":
            template[i][0] = template[i][0] % int((instrs[core])*(0.004))
          elif template[i][1][0]=="L2_directory.write_misses":
            template[i][0] = template[i][0] % int((instrs[core])*(0.0004))
          elif template[i][1][0]=="L2_directory.conflicts":
            template[i][0] = template[i][0] % int((instrs[core])*(0.00025))
          # L2 caches
          elif template[i][1][0]=="L2.read_accesses":
            #Z Fix: Dirty evictions will cause a read
            #Z Fix: Dirty backinvalidation 
            if add_implicit_accesses == True:
             template[i][0] = template[i][0] % (sum([ stats['L2.loads'][c] for c in cores_l2s ]) + sum([ stats['L2.dirty_evicts'][c] for c in cores_l2s ]) + sum([ stats['L2.dirty_backinval'][c] for c in cores_l2s ]))
            else:
             template[i][0] = template[i][0] % sum([ stats['L2.loads'][c] for c in cores_l2s ])
          elif template[i][1][0]=="L2.write_accesses":
            #Z Fix: Read misses will cause writes later
            #Z Fix: Writebacks are L2-hits, inclusive
            if add_implicit_accesses == True:
             template[i][0] = template[i][0] % (sum([ stats['L2.stores'][c] for c in cores_l2s ]) + sum([ stats['L2.load-misses'][c] for c in cores_l2s ]) + sum([ stats['L2.writebacks'][c] for c in cores_l2s ]))
            else:
             template[i][0] = template[i][0] % sum([ stats['L2.stores'][c] for c in cores_l2s ])
          elif template[i][1][0]=="L2.read_misses":
            template[i][0] = template[i][0] % sum([ stats['L2.load-misses'][c] for c in cores_l2s ])
          elif template[i][1][0]=="L2.write_misses":
            template[i][0] = template[i][0] % sum([ stats['L2.store-misses'][c] for c in cores_l2s ])
          elif template[i][1][0]=="L2.pic_copy_ops":
            template[i][0] = template[i][0] % ( 'L2.pic_ops_copy' in stats and sum([ stats['L2.pic_ops_copy'][c] for c in cores_l2s]) or 0)
          elif template[i][1][0]=="L2.pic_copy_ops_in_bank":
            if in_cache_frac != -1:
             template[i][0] = template[i][0] % ('L2.pic_ops_copy' in stats and sum( [(in_cache_frac * stats['L2.pic_ops_copy'][c]) for c in cores_l2s ]) or 0)
            else:
             template[i][0] = template[i][0] % ( 'L2.pic_ops_in_bank_copy_' + bank_map_policy in stats and sum([ stats['L2.pic_ops_in_bank_copy_' + bank_map_policy ][c] for c in cores_l2s ]) or 0)
          elif template[i][1][0]=="L2.pic_copy_tag_access":
            template[i][0] = template[i][0] % ( 'L2.pic_ops_tag_access_copy' in stats and sum([ stats['L2.pic_ops_tag_access_copy'][c] for c in cores_l2s ]) or 0)
          elif template[i][1][0]=="L2.pic_copy_read_miss":
            template[i][0] = template[i][0] % ( 'L2.pic_ops_read_miss_copy' in stats and sum([ stats['L2.pic_ops_read_miss_copy'][c] for c in cores_l2s ]) or 0)
          elif template[i][1][0]=="L2.pic_copy_write_miss":
            template[i][0] = template[i][0] % ( 'L2.pic_ops_write_miss_copy' in stats and sum([ stats['L2.pic_ops_write_miss_copy'][c] for c in cores_l2s ]) or 0)
          elif template[i][1][0]=="L2.pic_cmp_ops":
            template[i][0] = template[i][0] % long( \
															('L2.pic_ops_cmp' in stats and sum([ stats['L2.pic_ops_cmp'][c] for c in cores_l2s]) or 0)\
															+ ('L2.pic_ops_search' in stats and sum([ stats['L2.pic_ops_search'][c] for c in cores_l2s]) or 0))
          elif template[i][1][0]=="L2.pic_cmp_ops_in_bank":
            if in_cache_frac != -1:
             template[i][0] = template[i][0] % long( 'L2.pic_ops_cmp' in stats and \
		         ((sum([(in_cache_frac * stats['L2.pic_ops_cmp'][c]) for c in cores_l2s ]))\
							+ (sum([(in_cache_frac * stats['L2.pic_ops_search'][c]) for c in cores_l2s ]))) or 0)
            else:
             template[i][0] = template[i][0] % long ( \
             ('L2.pic_ops_in_bank_cmp_' + bank_map_policy in stats and sum([ stats['L2.pic_ops_in_bank_cmp_' + bank_map_policy ][c] for c in cores_l2s ]) or 0)\
             +('L2.pic_ops_in_bank_search_' + bank_map_policy in stats and sum([ stats['L2.pic_ops_in_bank_search_' + bank_map_policy ][c] for c in cores_l2s ]) or 0))
          elif template[i][1][0]=="L2.pic_cmp_tag_access":
            template[i][0] = template[i][0] % long ( \
                       ('L2.pic_ops_tag_access_cmp' in stats and sum([ stats['L2.pic_ops_tag_access_cmp'][c] for c in cores_l2s ]) or 0)\
                       +('L2.pic_ops_tag_access_search' in stats and sum([ stats['L2.pic_ops_tag_access_search'][c] for c in cores_l2s ]) or 0))
          elif template[i][1][0]=="L2.pic_cmp_read_miss":
            template[i][0] = template[i][0] % long ( \
											('L2.pic_ops_read_miss_cmp' in stats and sum([ stats['L2.pic_ops_read_miss_cmp'][c] for c in cores_l2s ]) or 0)\
											+('L2.pic_ops_read_miss_search' in stats and sum([ stats['L2.pic_ops_read_miss_search'][c] for c in cores_l2s ]) or 0))
          elif template[i][1][0]=="L2.pic_cmp_write_miss":
            template[i][0] = template[i][0] % long ( \
												('L2.pic_ops_write_miss_cmp' in stats and sum([ stats['L2.pic_ops_write_miss_cmp'][c] for c in cores_l2s ]) or 0)\
												+('L2.pic_ops_write_miss_search' in stats and sum([ stats['L2.pic_ops_write_miss_search'][c] for c in cores_l2s ]) or 0))
          elif template[i][1][0]=="L2.pic_key_writes":
            #if in_cache_frac == 0:
            # template[i][0] = template[i][0] % 0
            #else:
            template[i][0] = template[i][0] % ( 'L2.pic_key_writes' in stats and sum([ stats['L2.pic_key_writes'][c] for c in cores_l2s]) or 0)
          elif template[i][1][0]=="L2_duty_cycle":
            template[i][0] = template[i][0] % min(1,(sum([ stats['L2.loads'][c] + stats['L2.stores'][c] for c in cores_l2s ]) / float(max_system_cycles)))
          # L3 caches
          elif template[i][1][0]=="L3.read_accesses":
            template[i][0] = template[i][0] % ('L3.loads' in stats and sum([ stats['L3.loads'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.write_accesses":
            template[i][0] = template[i][0] % ('L3.stores' in stats and sum([ stats['L3.stores'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.read_misses":
            template[i][0] = template[i][0] % ('L3.load-misses' in stats and sum([ stats['L3.load-misses'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.write_misses":
            template[i][0] = template[i][0] % ('L3.store-misses' in stats and sum([ stats['L3.store-misses'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_copy_ops":
            template[i][0] = template[i][0] % ('L3.pic_ops_copy' in stats and sum([ stats['L3.pic_ops_copy'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_copy_ops_in_bank":
            template[i][0] = template[i][0] % ('L3.pic_ops_in_bank_copy_' + bank_map_policy in stats and sum([ stats['L3.pic_ops_in_bank_copy_' + bank_map_policy][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_copy_tag_access":
            template[i][0] = template[i][0] % ('L3.pic_ops_tag_access_copy' in stats and sum([ stats['L3.pic_ops_tag_access_copy'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_copy_read_miss":
            template[i][0] = template[i][0] % ('L3.pic_ops_read_miss_copy' in stats and sum([ stats['L3.pic_ops_read_miss_copy'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_copy_write_miss":
            template[i][0] = template[i][0] % ('L3.pic_ops_write_miss_copy' in stats and sum([ stats['L3.pic_ops_write_miss_copy'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_cmp_ops":
            template[i][0] = template[i][0] % ('L3.pic_ops_cmp' in stats and sum([ stats['L3.pic_ops_cmp'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_cmp_ops_in_bank":
            template[i][0] = template[i][0] % ('L3.pic_ops_in_bank_cmp_' + bank_map_policy in stats and sum([ stats['L3.pic_ops_in_bank_cmp_' + bank_map_policy][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_cmp_tag_access":
            template[i][0] = template[i][0] % ('L3.pic_ops_tag_access_cmp' in stats and sum([ stats['L3.pic_ops_tag_access_cmp'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_cmp_read_miss":
            template[i][0] = template[i][0] % ('L3.pic_ops_read_miss_cmp' in stats and sum([ stats['L3.pic_ops_read_miss_cmp'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_cmp_write_miss":
            template[i][0] = template[i][0] % ('L3.pic_ops_write_miss_cmp' in stats and sum([ stats['L3.pic_ops_write_miss_cmp'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3.pic_key_writes":
            #if in_cache_frac == 0:
            # template[i][0] = template[i][0] % 0
            #else:
            template[i][0] = template[i][0] % ('L3.pic_key_writes' in stats and sum([ stats['L3.pic_key_writes'][c] for c in cores_l3s ]) or 0)
          elif template[i][1][0]=="L3_duty_cycle":
            template[i][0] = template[i][0] % min(1,('L3.loads' in stats and sum([ stats['L3.loads'][c] + stats['L3.stores'][c] for c in cores_l3s ]) / float(max_system_cycles) or 0))
          else:
            raise ValueError('Unknown stat template %s' % template[i][1][0])
        elif template[i][1][1]=="comb":
          if template[i][1][0]=="icache_cfg":
            iconf=[]
            iconf.append(int(l1_icacheSize)*1024)
            iconf.append(l1_icacheBlockSize)
            iconf.append(l1_icacheAssociativity)
            iconf.append(1)
            iconf.append(1)              #thoughput="Cycle time of the component"
            iconf.append(latency_l1_i) #latency="access time"
            iconf.append(0) # unused?
            iconf.append(1) # 1 for writeback
            template[i][0] = template[i][0] % tuple(iconf)
          elif template[i][1][0]=="L2_config":
            l2conf=[]
            l2conf.append(int(l2_cacheSize)*1024)
            l2conf.append(l2_cacheBlockSize)
            l2conf.append(l2_cacheAssociativity)
            l2conf.append(8)
            l2conf.append(1)
            l2conf.append(latency_l2)
            l2conf.append(0) # unused?
            l2conf.append(1) # 1 for writeback
            template[i][0] = template[i][0] % tuple(l2conf)
          elif template[i][1][0]=="dcache_cfg":
            dconf=[]
            dconf.append(int(l1_dcacheSize)*1024)
            dconf.append(l1_dcacheBlockSize)
            dconf.append(l1_dcacheAssociativity)
            dconf.append(2)            #banks
            # Increase throughput and latency constraints, otherwise McPAT calls CACTI some more
            #   with tighter constraints, resulting in a ridiculously large dcache
            dconf.append(10)            #thoughput="Cycle time of the component"
            dconf.append(10*latency_l1_d)
            dconf.append(0) # unused?
            dconf.append(1) # 1 for writeback
            template[i][0] = template[i][0] % tuple(dconf)
          elif template[i][1][0]=="L3_config":
            l3conf=[]
            l3conf.append(int(l3_cacheSize)*1024)
            l3conf.append(64)
            l3conf.append(l3_cacheAssociativity)
            l3conf.append(16)
            l3conf.append(16)
            l3conf.append(latency_l3)
            l3conf.append(1)
            template[i][0] = template[i][0] % tuple(l3conf)
  return template, nuca_at_level
#----------
def readTemplate(ncores, num_l2s, private_l2s, num_l3s, technology_node):
  Count = 0
  template=[]
  template.append(["<?xml version=\"1.0\" ?>",""])
  template.append(["<!-- McPAT interface-->",""])
  template.append(["<component id=\"root\" name=\"root\">",""])
  template.append(["\t<component id=\"system\" name=\"system\">",""])
  template.append(["\t\t<!--McPAT will skip the components if number is set to 0 -->",""])
  template.append(["\t\t<param name=\"number_of_cores\" value=\"%i\"/>"%ncores,""])
  template.append(["\t\t<param name=\"number_of_L1Directories\" value=\"0\"/>",""])
  template.append(["\t\t<param name=\"number_of_L2Directories\" value=\"0\"/>",""])
  template.append(["\t\t<param name=\"number_of_L2s\" value=\"%i\"/> <!-- This number means how many L2 clusters in each cluster there can be multiple banks/ports -->"%num_l2s,""])
  template.append(["\t\t<param name=\"Private_L2\" value =\"%i\"/>"%(1 if private_l2s else 0),""])
  template.append(["\t\t<param name=\"number_of_L3s\" value=\"%i\"/> <!-- This number means how many L3 clusters -->"%num_l3s,""])
  template.append(["\t\t<param name=\"number_of_NoCs\" value=\"1\"/>",""])
  template.append(["\t\t<param name=\"homogeneous_cores\" value=\"0\"/><!--1 means homo -->",""])
  template.append(["\t\t<param name=\"homogeneous_L2s\" value=\"0\"/>",""])
  template.append(["\t\t<param name=\"homogeneous_L1Directories\" value=\"0\"/>",""])
  template.append(["\t\t<param name=\"homogeneous_L2Directories\" value=\"0\"/>",""])
  template.append(["\t\t<param name=\"homogeneous_L3s\" value=\"0\"/>",""])
  template.append(["\t\t<param name=\"homogeneous_ccs\" value=\"1\"/><!--cache coherece hardware -->",""])
  template.append(["\t\t<param name=\"homogeneous_NoCs\" value=\"1\"/>",""])
  template.append(["\t\t<param name=\"core_tech_node\" value=\"%u\"/><!-- nm -->"%technology_node,""])
  template.append(["\t\t<param name=\"target_core_clockrate\" value='%i'/><!--MHz -->",["core_clock","cfg",None]]) #CFG
  template.append(["\t\t<param name=\"temperature\" value=\"330\"/> <!-- Kelvin -->",""])
  template.append(["\t\t<param name=\"number_cache_levels\" value=\"3\"/>",""])
  template.append(["\t\t<param name=\"interconnect_projection_type\" value=\"0\"/><!--0: agressive wire technology; 1: conservative wire technology -->",""])
  template.append(["\t\t<param name=\"device_type\" value=\"0\"/><!--0: HP(High Performance Type); 1: LSTP(Low standby power) 2: LOP (Low Operating Power)  -->",""])
  template.append(["\t\t<param name=\"longer_channel_device\" value=\"1\"/><!-- 0 no use; 1 use when approperiate -->",""])
  template.append(["\t\t<param name=\"power_gating\" value=\"1\"/><!-- 0 not enabled; 1 enabled -->",""])
  template.append(["\t\t<param name=\"machine_bits\" value=\"64\"/>",""])
  template.append(["\t\t<param name=\"virtual_address_width\" value=\"64\"/>",""])
  template.append(["\t\t<param name=\"physical_address_width\" value=\"52\"/>",""])     #
  template.append(["\t\t<param name=\"virtual_memory_page_size\" value=\"4096\"/>",""]) #
#--------------------------------------------------------------------------------------------------------------------
  template.append(["\t\t<stat name=\"total_cycles\" value=\"%i\"/>",["total_system_cycles","stat",-1]])     #STATS
  template.append(["\t\t<stat name=\"idle_cycles\" value=\"%i\"/>",["total_system_idle_cycles","stat",-1]])  #STATS
  template.append(["\t\t<stat name=\"busy_cycles\"  value=\"%i\"/>",["total_system_busy_cycles","stat",-1]])      #STATS
#--------------------------------------------------------------------------------------------------------------------
  template.append(["\t\t\t<!--This page size(B) is complete different from the page size in Main memo secction. this page size is the size of ",""])
  template.append(["\t\t\tvirtual memory from OS/Archi perspective; the page size in Main memo secction is the actuall physical line in a DRAM bank  -->",""])
  template.append(["\t\t<!-- *********************** cores ******************* -->",""])
#################################################################################################################
  for iCount in range(ncores):
    template.append(["\t\t<component id=\"system.core%i\" name=\"core%i\">"%(iCount,iCount),""])  #check how this can be done/
    template.append(["\t\t<!-- Core property -->",""])
#       template.append(["\t\t\t<param name=\"clock_rate\" value=\"2660\"/>",""])       #CFG
    template.append(["\t\t\t<param name=\"clock_rate\" value='%i'/>",["core_clock","cfg",iCount]])   #CFG
    template.append(["\t\t\t<param name=\"vdd\" value=\"%f\"/><!-- 0 means using ITRS default vdd -->",["core_vdd","cfg",iCount]])
    template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
    template.append(["\t\t\t<param name=\"opt_local\" value=\"1\"/>",""])
    template.append(["\t\t\t<param name=\"instruction_length\" value=\"32\"/>",""])
    template.append(["\t\t\t<param name=\"opcode_width\" value=\"16\"/>",""])
    template.append(["\t\t\t<!-- address width determins the tag_width in Cache, LSQ and buffers in cache controller ",""])
    template.append(["\t\t\tdefault value is machine_bits, if not set --> ",""])
    template.append(["\t\t\t<param name=\"machine_type\" value=\"%i\"/><!-- 1 inorder; 0 OOO-->",["machineType","cfg",iCount]])
    template.append(["\t\t\t<!-- inorder/OoO -->",""])
    template.append(["\t\t\t<param name=\"number_hardware_threads\" value=\"1\"/>",""])
    template.append(["\t\t\t<!-- number_instruction_fetch_ports(icache ports) is always 1 in single-thread processor,",""])
    template.append(["\t\t\tit only may be more than one in SMT processors. BTB ports always equals to fetch ports since ",""])
    template.append(["\t\t\tbranch information in consective branch instructions in the same fetch group can be read out from BTB once.--> ",""])
    template.append(["\t\t\t<param name=\"fetch_width\" value=\"%d\"/>",["issue_width","cfg",iCount]])
    template.append(["\t\t\t<!-- fetch_width determins the size of cachelines of L1 cache block -->",""])
    template.append(["\t\t\t<param name=\"number_instruction_fetch_ports\" value=\"1\"/>",""])
    template.append(["\t\t\t<param name=\"decode_width\" value=\"%d\"/>",["issue_width","cfg",iCount]])
    template.append(["\t\t\t<!-- decode_width determins the number of ports of the ",""])
    template.append(["\t\t\trenaming table (both RAM and CAM) scheme -->",""])
#---------------------------------------------------------------------------
    template.append(["\t\t\t<param name=\"x86\" value=\"1\"/> ",""])
    template.append(["\t\t\t<param name=\"micro_opcode_width\" value=\"8\"/> ",""])

#---------------------------------------------------------------------------
    template.append(["\t\t\t<param name=\"issue_width\" value=\"%u\"/>" ,["issue_width","cfg",iCount]])
    template.append(["\t\t\t<param name=\"peak_issue_width\" value=\"%u\"/>",["peak_issue_width","cfg",iCount]])
    template.append(["\t\t\t<!-- issue_width determins the number of ports of Issue window and other logic ",""])
    template.append(["\t\t\t\tas in the complexity effective proccessors paper; issue_width==dispatch_width -->",""])
    template.append(["\t\t\t<param name=\"commit_width\" value=\"%u\"/>",["issue_width","cfg",iCount]]) #assuming the issue and commit widths are equal
    template.append(["\t\t\t<!-- commit_width determins the number of ports of register files -->",""])
    template.append(["\t\t\t<param name=\"fp_issue_width\" value=\"2\"/>",""])
    template.append(["\t\t\t<param name=\"prediction_width\" value=\"1\"/>",""])  #Check whether it can be extracted
    template.append(["\t\t\t<!-- number of branch instructions can be predicted simultannouesl-->",""])
    template.append(["\t\t\t<!-- Current version of McPAT does not distinguish int and floating point pipelines",""])
    template.append(["\t\t\tTheses parameters are reserved for future use.-->",""])
    template.append(["\t\t\t<param name=\"pipelines_per_core\" value=\"1,1\"/>",""])      # "#int_pipelines,#fp_pipelines"; if fp_pipelines=0 they are shared
    template.append(["\t\t\t<!--integer_pipeline and floating_pipelines, if the floating_pipelines is 0, then the pipeline is shared-->",""])
        #template.append(["\t\t\t<param name=\"pipeline_depth\" value=\"%i,%i\"/>",["pipe_depth","comb"]])              #Check how it can be extracted
    template.append(["\t\t\t<param name=\"pipeline_depth\" value=\"14,14\"/>",""])                #Check how it can be extracted
    template.append(["\t\t\t<!-- pipeline depth of int and fp, if pipeline is shared, the second number is the average cycles of fp ops -->",""])
    template.append(["\t\t\t<!-- issue and exe unit-->",""])
    template.append(["\t\t\t<param name=\"ALU_per_core\" value=\"%u\"/>",["ALU_per_core","cfg",iCount],""])
    template.append(["\t\t\t<param name=\"MUL_per_core\" value=\"1\"/>",""])
    template.append(["\t\t\t<!-- In superscalar processors, usually all ALU are not the same. certain inst. can only",""])
    template.append(["\t\t\tbe processed by certain ALU. However, current McPAT does not consider this subtle difference -->",""])
    template.append(["\t\t\t<param name=\"FPU_per_core\" value=\"2\"/>",""])
    template.append(["\t\t\t<!-- buffer between IF and ID stage -->",""])
    template.append(["\t\t\t<param name=\"instruction_buffer_size\" value=\"32\"/>",""])
    template.append(["\t\t\t<!-- buffer between ID and sche/exe stage -->",""])
    template.append(["\t\t\t<param name=\"decoded_stream_buffer_size\" value=\"16\"/>",""])
    template.append(["\t\t\t<param name=\"instruction_window_scheme\" value=\"1\"/><!-- 0 PHYREG based, 1 RSBASED-->",""])
    template.append(["\t\t\t<!-- McPAT support 2 types of OoO cores, RS based and physical reg based-->",""])
    template.append(["\t\t\t<param name=\"instruction_window_size\" value=\"36\"/>",""])  #
    template.append(["\t\t\t<param name=\"fp_instruction_window_size\" value=\"0\"/>",""])       #
    template.append(["\t\t\t<!-- the instruction issue Q as in Alpha 21264; The RS as in Intel P6 -->",""])
    template.append(["\t\t\t<param name=\"ROB_size\" value=\"%d\"/>",["window_size","cfg",iCount]])
    template.append(["\t\t\t<!-- each in-flight instruction has an entry in ROB -->",""])
    template.append(["\t\t\t<!-- registers -->",""])
    template.append(["\t\t\t<param name=\"archi_Regs_IRF_size\" value=\"16\"/>",""])
    template.append(["\t\t\t<param name=\"archi_Regs_FRF_size\" value=\"32\"/>",""])
    template.append(["\t\t\t<!--  if OoO processor, phy_reg number is needed for renaming logic, ",""])
    template.append(["\t\t\trenaming logic is for both integer and floating point insts.  -->",""])
    template.append(["\t\t\t<param name=\"phy_Regs_IRF_size\" value=\"256\"/>",""])               #numbers by default
    template.append(["\t\t\t<param name=\"phy_Regs_FRF_size\" value=\"256\"/>",""])               #numbers by default
    template.append(["\t\t\t<!-- rename logic -->",""])
    template.append(["\t\t\t<param name=\"rename_scheme\" value=\"0\"/>",""])             #0 originally
    template.append(["\t\t\t<!-- can be RAM based(0) or CAM based(1) rename scheme",""])
    template.append(["\t\t\tRAM-based scheme will have free list, status table;",""])
    template.append(["\t\t\tRAM-based scheme have the valid bit in the data field of the CAM ",""])
    template.append(["\t\t\tboth RAM and CAM need RAM-based checkpoint table, checkpoint_depth=# of in_flight instructions;",""])
    template.append(["\t\t\tDetailed RAT Implementation see TR -->",""])
    template.append(["\t\t\t<param name=\"register_windows_size\" value=\"0\"/>",""])
    template.append(["\t\t\t<!-- how many windows in the windowed register file, sun processors;",""])
    template.append(["\t\t\tno register windowing is used when this number is 0 -->",""])
    template.append(["\t\t\t<!-- In OoO cores, loads and stores can be issued whether inorder(Pentium Pro) or (OoO)out-of-order(Alpha),",""])
    template.append(["\t\t\tThey will always try to exeute out-of-order though. -->",""])
    template.append(["\t\t\t<param name=\"LSU_order\" value=\"inorder\"/>",""])
    template.append(["\t\t\t<param name=\"store_buffer_size\" value=\"96\"/>",""])                #
    template.append(["\t\t\t<!-- By default, in-order cores do not have load buffers -->",""])
    template.append(["\t\t\t<param name=\"load_buffer_size\" value=\"48\"/>",""])
    template.append(["\t\t\t<!-- number of ports refer to sustainable concurrent memory accesses -->",""])
    template.append(["\t\t\t<param name=\"memory_ports\" value=\"1\"/>",""])      #Would this be 1 or 2
    template.append(["\t\t\t<!-- max_allowed_in_flight_memo_instructions determins the # of ports of load and store buffer",""])
    template.append(["\t\t\tas well as the ports of Dcache which is connected to LSU -->",""])
    template.append(["\t\t\t<!-- dual-pumped Dcache can be used to save the extra read/write ports -->",""])
#       template.append(["\t\t\t<param name=\"RAS_size\" value='%i'/>",["system.cpu.RASSize","cfg"]])
    template.append(["\t\t\t<param name=\"RAS_size\" value=\"64\"/>",""])
    template.append(["\t\t\t<!-- general stats, defines simulation periods;require total, idle, and busy cycles for senity check  -->",""])
    template.append(["\t\t\t<!-- please note: if target architecture is X86, then all the instrucions refer to (fused) micro-ops -->",""])
#----------------------------------------------------------------------------------------------------------------------------
#Important stats to be extracted
    template.append(["\t\t\t<stat name=\"total_instructions\" value=\"%i\"/>",["total_instructions","stat",iCount]])       #stats --
    template.append(["\t\t\t<stat name=\"int_instructions\" value=\"%i\"/>",["integer_ins","stat",iCount]])        #stats--
    template.append(["\t\t\t<stat name=\"fp_instructions\" value=\"%i\"/>",["fp_ins","stat",iCount]])  #stats--
    template.append(["\t\t\t<stat name=\"branch_instructions\" value=\"%i\"/>",["branch_ins","stat",iCount]])  #stats--
    template.append(["\t\t\t<stat name=\"branch_mispredictions\" value=\"%i\"/>",["branch_mis","stat",iCount]])  #stats--
    template.append(["\t\t\t<stat name=\"load_instructions\" value=\"%i\"/>",["loads","stat",iCount]])  #stats
    template.append(["\t\t\t<stat name=\"store_instructions\" value=\"%i\"/>",["stores","stat",iCount]])  #stats
    template.append(["\t\t\t<stat name=\"committed_instructions\" value=\"%i\"/>",["total_instructions","stat",iCount]])  #stats
    template.append(["\t\t\t<stat name=\"committed_int_instructions\" value=\"%i\"/>",["integer_ins","stat",iCount]])  #stats
    template.append(["\t\t\t<stat name=\"committed_fp_instructions\" value=\"%i\"/>",["fp_ins","stat",iCount]])  #stats
    template.append(["\t\t\t<stat name=\"total_cycles\" value=\"%i\"/>",["total_cycles","stat",iCount]])  #stats
    #template.append(["\t\t\t<stat name=\"idle_cycles\" value=\"%i\"/>",["","stat",iCount]])  #stats
    template.append(["\t\t\t<stat name=\"idle_cycles\" value=\"%i\"/>",["idle_cycles","stat",iCount]])  #stats
    template.append(["\t\t\t<stat name=\"busy_cycles\"  value=\"%i\"/>",["busy_cycles","stat",iCount]])
#----------------------------------------------------------------------------------------------------------------------------
    template.append(["\t\t\t<!-- instruction buffer stats -->",""])
    template.append(["\t\t\t<!-- ROB stats, both RS and Phy based OoOs have ROB",""])
    template.append(["\t\t\tperformance simulator should capture the difference on accesses,",""])
    template.append(["\t\t\totherwise, McPAT has to guess based on number of commited instructions. -->",""])
    template.append(["\t\t\t<stat name=\"ROB_reads\" value=\"%i\"/>",["ROB_reads","stat",iCount]])
    template.append(["\t\t\t<stat name=\"ROB_writes\" value=\"%i\"/>",["ROB_writes","stat",iCount]])
    template.append(["\t\t\t<!-- RAT accesses -->",""])
    template.append(["\t\t\t<stat name=\"rename_reads\" value=\"%i\"/>",["RAT_rename.reads","stat",iCount]])
    template.append(["\t\t\t<stat name=\"rename_writes\" value=\"%i\"/>",["RAT_rename.writes","stat",iCount]])
    template.append(["\t\t\t<stat name=\"fp_rename_reads\" value=\"%i\"/>",["RAT_fp_rename.reads","stat",iCount]])
    template.append(["\t\t\t<stat name=\"fp_rename_writes\" value=\"%i\"/>",["RAT_fp_rename.writes","stat",iCount]])
    template.append(["\t\t\t<!-- decode and rename stage use this, should be total ic - nop -->",""])
    template.append(["\t\t\t<!-- Inst window stats -->",""])
    template.append(["\t\t\t<stat name=\"inst_window_reads\" value=\"%i\"/>",["instr.reads","stat",iCount]])     #left to defaults
    template.append(["\t\t\t<stat name=\"inst_window_writes\" value=\"%i\"/>",["instr.writes","stat",iCount]])    #left to defaults
    template.append(["\t\t\t<stat name=\"inst_window_wakeup_accesses\" value=\"%i\"/>",["instr.wakeup","stat",iCount]])   #left to defaults
    template.append(["\t\t\t<stat name=\"fp_inst_window_reads\" value=\"%i\"/>",["instr.fp.reads","stat",iCount]])  #left to defaults
    template.append(["\t\t\t<stat name=\"fp_inst_window_writes\" value=\"%i\"/>",["instr.fp.writes","stat",iCount]]) #left to defaults
    template.append(["\t\t\t<stat name=\"fp_inst_window_wakeup_accesses\" value=\"%i\"/>",["instr.fp.wakeup","stat",iCount]])        #left to defaults'''
#--------------------------------------------------------------------------------------------------------------------
    template.append(["\t\t\t<!--  RF accesses -->",""])
    template.append(["\t\t\t<stat name=\"int_regfile_reads\" value=\"%i\"/>",["RF_accesses.int_regfile_reads","stat",iCount]])
    template.append(["\t\t\t<stat name=\"float_regfile_reads\" value=\"%i\"/>",["RF_accesses.fp_regfile_reads","stat",iCount]])
    template.append(["\t\t\t<stat name=\"int_regfile_writes\" value=\"%i\"/>",["RF_accesses.int_regfile_writes","stat",iCount]])
    template.append(["\t\t\t<stat name=\"float_regfile_writes\" value=\"%i\"/>",["RF_accesses.fp_regfile_writes","stat",iCount]])
    template.append(["\t\t\t<!-- accesses to the working reg -->",""])
    #template.append(["\t\t\t<stat name=\"function_calls\" value=\"%i\"/>",["function_call","stat"]])     #check regarding the stats
    template.append(["\t\t\t<stat name=\"function_calls\" value=\"%i\"/>",["function_calls","stat",iCount]])     #check regarding the stats
    template.append(["\t\t\t<stat name=\"context_switches\" value=\"0\"/>",""])
    template.append(["\t\t\t<!-- Number of Windowes switches (number of function calls and returns)-->",""])
    template.append(["\t\t\t<!-- Alu stats by default, the processor has one FPU that includes the divider and ",""])
    template.append(["\t\t\t multiplier. The fpu accesses should include accesses to multiplier and divider  -->",""])

    template.append(["\t\t\t<stat name=\"ialu_access\" value=\"%i\"/>",["window_switches.ialu_accesses","stat",iCount]])           #check regarding the stats
    template.append(["\t\t\t<stat name=\"fpu_access\" value=\"%i\"/>",["window_switches.fpu_accesses","stat",iCount]])    #check regarding the stats
    template.append(["\t\t\t<stat name=\"mul_accesses\" value=\"%i\"/>",["window_switches.mul_accesses","stat",iCount]])  #check regarding the stats
    template.append(["\t\t\t<stat name=\"cdb_alu_accesses\" value=\"%i\"/>",["window_switches.cdb_alu_accesses","stat",iCount]])      #check regarding the stats
    template.append(["\t\t\t<stat name=\"cdb_mul_accesses\" value=\"%i\"/>",["window_switches.cdb_mul_accesses","stat",iCount]])      #check regarding the stats
    template.append(["\t\t\t<stat name=\"cdb_fpu_accesses\" value=\"%i\"/>",["window_switches.cdb_fpu_accesses","stat",iCount]])      #check regarding the stats
    template.append(["\t\t\t<!-- multiple cycle accesses should be counted multiple times, ",""])
    template.append(["\t\t\totherwise, McPAT can use internal counter for different floating point instructions ",""])
    template.append(["\t\t\tto get final accesses. But that needs detailed info for floating point inst mix -->",""])
    template.append(["\t\t\t<!--  currently the performance simulator should ",""])
    template.append(["\t\t\tmake sure all the numbers are final numbers, ",""])
    template.append(["\t\t\tincluding the explicit read/write accesses, ",""])
    template.append(["\t\t\tand the implicite accesses such as replacements and etc.",""])
    template.append(["\t\t\tFuture versions of McPAT may be able to reason the implicite access",""])
    template.append(["\t\t\tbased on param and stats of last level cache",""])
    template.append(["\t\t\tThe same rule applies to all cache access stats too!  -->",""])
#--------------------------------------------------------------------------------------------------------

    template.append(["\t\t\t<stat name=\"IFU_duty_cycle\" value=\"%f\"/>",["IFU.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"LSU_duty_cycle\" value=\"%f\"/>",["LSU.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"MemManU_I_duty_cycle\" value=\"%f\"/>",["IFU.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"MemManU_D_duty_cycle\" value=\"%f\"/>",["MemManU.D.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"ALU_duty_cycle\" value=\"%f\"/>",["ALU.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"MUL_duty_cycle\" value=\"%f\"/>",["MUL.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"FPU_duty_cycle\" value=\"%f\"/>",["FPU.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"ALU_cdb_duty_cycle\" value=\"%f\"/>",["ALU.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"MUL_cdb_duty_cycle\" value=\"%f\"/>",["MUL.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<stat name=\"FPU_cdb_duty_cycle\" value=\"%f\"/>",["FPU.duty_cycle","stat",iCount]])
    template.append(["\t\t\t<param name=\"number_of_BPT\" value=\"2\"/>",""])

#-------------------------------------------------------------------------------------------------------

    template.append(["\t\t\t<component id=\"system.core%i.predictor\" name=\"PBT\">"%iCount,""])
    template.append(["\t\t\t\t<!-- branch predictor; tournament predictor see Alpha implementation -->",""])
    template.append(["\t\t\t\t<param name=\"local_predictor_size\" value=\"10,3\"/>",""])         #????????????
#this section is hardcoded which will later be replaced once the parameters are extracted from graphite
    template.append(["\t\t\t\t<param name=\"local_predictor_entries\" value=\"1024\"/>",""])
    template.append(["\t\t\t\t<param name=\"global_predictor_entries\" value=\"4096\"/>",""])
    template.append(["\t\t\t\t<param name=\"global_predictor_bits\" value=\"2\"/>",""])
    template.append(["\t\t\t\t<param name=\"chooser_predictor_entries\" value=\"4096\"/>",""])
    template.append(["\t\t\t\t<param name=\"chooser_predictor_bits\" value=\"2\"/>",""])

#-------------------------------------------------------------------------------------------------------------------------
    template.append(["\t\t\t</component>",""])
    template.append(["\t\t\t<component id=\"system.core%i.itlb\" name=\"itlb\">"%iCount,""])
    template.append(["\t\t\t\t<param name=\"number_entries\" value=\"128\"/>",""])                #not in graphite (whether correct)
    template.append(["\t\t\t\t<stat name=\"total_accesses\" value=\"%i\"/>",["itlb.total_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"total_misses\" value=\"%i\"/>",["itlb.total_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<!-- there is no write requests to itlb although writes happen to itlb after miss,",""])
    template.append(["\t\t\t\twhich is actually a replacement -->",""])
    template.append(["\t\t\t</component>",""])
    template.append(["\t\t\t<component id=\"system.core%i.icache\" name=\"icache\">"%iCount,""])
    template.append(["\t\t\t\t<!-- there is no write requests to itlb although writes happen to it after miss, ",""])
    template.append(["\t\t\t\twhich is actually a replacement -->",""])
    template.append(["\t\t\t\t<param name=\"icache_config\" value=\"%i,%i,%i,%i,%i,%i, %i, %i\"/>",["icache_cfg","comb",iCount]])        #calculate this properly
    #template.append(["\t\t\t\t<param name=\"icache_config\" value=\"32768,32,8,1,4,4,32,0 \"/>",""])        #the above icache config hardcoded

    template.append(["\t\t\t\t<!-- the parameters are capacity,block_width, associativity,bank, throughput w.r.t. core clock, latency w.r.t. core clock,-->",""])
#       template.append(["\t\t\t\t<param name=\"buffer_sizes\" value=\"%i, 16, %i, %i\"/>",["icache_buffer","comb"]]) #mcpat will crash for some different sizes of 2nd parameter
    template.append(["\t\t\t\t<param name=\"buffer_sizes\" value=\"16, 16, 16, 0\"/>",""]) #mcpat will crash for some different sizes of 2nd parameter
    template.append(["\t\t\t\t<!-- cache controller buffer sizes: miss_buffer_size(MSHR),fill_buffer_size,prefetch_buffer_size,wb_buffer_size-->",""] )
    template.append(["\t\t\t\t<stat name=\"read_accesses\" value=\"%i\"/>",["icache.read_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"read_misses\" value=\"%i\"/>",["icache.read_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"0\"/>",""] )
    template.append(["\t\t\t</component>",""])
    template.append(["\t\t\t<component id=\"system.core%i.dtlb\" name=\"dtlb\">"%iCount,""])
    template.append(["\t\t\t\t<param name=\"number_entries\" value=\"256\"/>",""])
    template.append(["\t\t\t\t<stat name=\"total_accesses\" value=\"%i\"/>",["dtlb.total_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"total_misses\" value=\"%i\"/>",["dtlb.total_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"0\"/>",""])
    template.append(["\t\t\t</component>",""])
    template.append(["\t\t\t<component id=\"system.core%i.dcache\" name=\"dcache\">"%iCount,""])
    template.append(["\t\t\t\t<!-- all the buffer related are optional -->",""])
    template.append(["\t\t\t\t<param name=\"dcache_config\" value=\"%i,%i,%i,%i,%i,%i, %i, %i\"/>",["dcache_cfg","comb",iCount]])
    #template.append(["\t\t\t\t<param name=\"dcache_config\" value=\"32768,32,8,1, 4,6, 32,1 \"/>",""])
    template.append(["\t\t\t\t<param name=\"buffer_sizes\" value=\"16, 16, 16, 16\"/>",""]) #mcpat will crash for some different sizes of 2nd parameter
    template.append(["\t\t\t\t<!-- cache controller buffer sizes: miss_buffer_size(MSHR),fill_buffer_size,prefetch_buffer_size,wb_buffer_size-->",""])
    template.append(["\t\t\t\t<stat name=\"read_accesses\" value=\"%i\"/>",["dcache.read_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_accesses\" value=\"%i\"/>",["dcache.write_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"read_misses\" value=\"%i\"/>",["dcache.read_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_misses\" value=\"%i\"/>",["dcache.write_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<stat name=\"pic_copy_ops\" value=\"%i\"/>",["dcache.pic_copy_ops","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_ops_in_bank\" value=\"%i\"/>",["dcache.pic_copy_ops_in_bank","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_tag_access\" value=\"%i\"/>",["dcache.pic_copy_tag_access","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_read_miss\" value=\"%i\"/>",["dcache.pic_copy_read_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_write_miss\" value=\"%i\"/>",["dcache.pic_copy_write_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_ops\" value=\"%i\"/>",["dcache.pic_cmp_ops","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_ops_in_bank\" value=\"%i\"/>",["dcache.pic_cmp_ops_in_bank","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_tag_access\" value=\"%i\"/>",["dcache.pic_cmp_tag_access","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_read_miss\" value=\"%i\"/>",["dcache.pic_cmp_read_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_write_miss\" value=\"%i\"/>",["dcache.pic_cmp_write_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_search_key_writes\" value=\"%i\"/>",["dcache.pic_key_writes","stat",iCount]])
    template.append(["\t\t\t</component>",""])
    template.append(["\t\t\t<component id=\"system.core%i.BTB\" name=\"BTB\">"%iCount,""])
    template.append(["\t\t\t\t<!-- all the buffer related are optional -->",""])
    template.append(["\t\t\t\t<param name=\"BTB_config\" value=\"18944,8,4,1, 1,3\"/>",""]) # ( (64 target + 3 type + 4 ffset + 3 PLRU bits) * 512 entries * 4 ways ) / 8 bits-per-byte = 18944 bytes; tag overheads are already taken into account; block size = 8, associativity = 4, num-of-banks = 1, throughput = 1, latency = 3
    template.append(["\t\t\t\t<stat name=\"read_accesses\" value=\"%i\"/>",["BTB.read_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_accesses\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<!-- the parameters are capacity,block_width,associativity,bank, throughput w.r.t. core clock, latency w.r.t. core clock,-->",""])
    template.append(["\t\t\t</component>",""])
    template.append(["\t</component>",""])
    template.append(["\t\t\t<!--**********************************************************************-->",""])

  if 0: # not currently modeling any L1 directory
    template.append(["\t\t<component id=\"system.L1Directory%i\" name=\"L1Directory%i\">"%(iCount,iCount),""])
    template.append(["\t\t\t\t<param name=\"Directory_type\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<param name=\"Dir_config\" value=\"4096,2,0,1,100,100, 8\"/>",""])
    template.append(["\t\t\t\t<param name=\"buffer_sizes\" value=\"8, 8, 8, 8\"/>",""])
    template.append(["\t\t\t\t<param name=\"clockrate\" value=\"%i\"/>",["core_clock","cfg",iCount]])
    template.append(["\t\t\t\t<param name=\"ports\" value=\"1,1,1\"/>",""])
    template.append(["\t\t\t\t<param name=\"device_type\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<!-- altough there are multiple access types, Performance simulator needs to cast them into reads or writes         e.g. the invalidates can be considered as writes -->",""])
    template.append(["\t\t\t\t<stat name=\"read_accesses\" value=\"%i\"/>",["L1_directory.read_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_accesses\" value=\"%i\"/>",["L1_directory.write_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"read_misses\" value=\"%i\"/>",["L1_directory.read_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_misses\" value=\"%i\"/>",["L1_directory.write_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"%i\"/>",["L1_directory.conflicts","stat",iCount]])
    template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------------
  if 0: # not currently modeling any L2 directory
    template.append(["\t\t<component id=\"system.L2Directory%i\" name=\"L2Directory%i\">"%(iCount,iCount),""])
    template.append(["\t\t\t\t<param name=\"Directory_type\" value=\"1\"/>",""])
    template.append(["\t\t\t\t<param name=\"Dir_config\" value=\"1048576,16,16,1,2, 100\"/>",""])
    template.append(["\t\t\t\t<param name=\"buffer_sizes\" value=\"8, 8, 8, 8\"/>",""])
    template.append(["\t\t\t\t<param name=\"clockrate\" value=\"%i\"/>",["core_clock","cfg",iCount]])
    template.append(["\t\t\t\t<param name=\"ports\" value=\"1,1,1\"/>",""])
    template.append(["\t\t\t\t<param name=\"device_type\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<!-- altough there are multiple access types, Performance simulator needs to cast them into reads or writes         e.g. the invalidates can be considered as writes -->",""])
    template.append(["\t\t\t\t<stat name=\"read_accesses\" value=\"%i\"/>",["L2_directory.read_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_accesses\" value=\"%i\"/>",["L2_directory.write_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"read_misses\" value=\"%i\"/>",["L2_directory.read_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_misses\" value=\"%i\"/>",["L2_directory.write_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"%i\"/>",["L2_directory.conflicts","stat",iCount]])
    template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------------
  for iCount in range(num_l2s):
    template.append(["\t\t<component id=\"system.L2%i\" name=\"L2%i\">"%(iCount,iCount),""])
    template.append(["\t\t\t\t<param name=\"L2_config\" value=\"%i,%i,%i,%i,%i,%i, %i, %i\"/>",["L2_config","comb",iCount]])
    #template.append(["\t\t\t\t<param name=\"L2_config\" value=\"6291456,64, 16, 8, 8, 23, 32, 1\"/>",""])
    template.append(["\t\t\t\t<param name=\"buffer_sizes\" value=\"16, 16, 16, 16\"/>",""])
    template.append(["\t\t\t\t<param name=\"clockrate\" value=\"%i\"/>",["L2_clock","cfg",iCount]])
    template.append(["\t\t\t\t<param name=\"vdd\" value=\"%f\"/><!-- 0 means using ITRS default vdd -->",["L2_vdd","cfg",iCount]])
    template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
    template.append(["\t\t\t\t<param name=\"ports\" value=\"1,1,1\"/>",""])
    template.append(["\t\t\t\t<param name=\"device_type\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<stat name=\"read_accesses\" value=\"%i\"/>",["L2.read_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_accesses\" value=\"%i\"/>",["L2.write_accesses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"read_misses\" value=\"%i\"/>",["L2.read_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"write_misses\" value=\"%i\"/>",["L2.write_misses","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_ops\" value=\"%i\"/>",["L2.pic_copy_ops","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_ops_in_bank\" value=\"%i\"/>",["L2.pic_copy_ops_in_bank","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_tag_access\" value=\"%i\"/>",["L2.pic_copy_tag_access","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_read_miss\" value=\"%i\"/>",["L2.pic_copy_read_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_write_miss\" value=\"%i\"/>",["L2.pic_copy_write_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_ops\" value=\"%i\"/>",["L2.pic_cmp_ops","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_ops_in_bank\" value=\"%i\"/>",["L2.pic_cmp_ops_in_bank","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_tag_access\" value=\"%i\"/>",["L2.pic_cmp_tag_access","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_read_miss\" value=\"%i\"/>",["L2.pic_cmp_read_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_write_miss\" value=\"%i\"/>",["L2.pic_cmp_write_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_search_key_writes\" value=\"%i\"/>",["L2.pic_key_writes","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<stat name=\"duty_cycle\" value=\"%f\"/>",["L2_duty_cycle","stat",iCount]])
    template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------------
  for iCount in range(num_l3s):
    template.append(["\t\t<component id=\"system.L3%i\" name=\"L3%i\">"%(iCount,iCount),""])
    template.append(["\t\t\t\t<param name=\"L3_config\" value=\"%i,%i,%i, %i, %i, %i,%i\"/>",["L3_config","comb",iCount]])
    #template.append(["\t\t\t<param name=\"L3_config\" value=\"16777216 , 64 ,16, 16, 16, 100,1\"/>",""])
    template.append(["\t\t<!-- the parameters are capacity,block_width, associativity,bank, throughput w.r.t. core clock, latency w.r.t. core clock,-->",""])
    template.append(["\t\t\t\t<param name=\"clockrate\" value=\"%i\"/>",["L3_clock","cfg",iCount]])
    template.append(["\t\t\t\t<param name=\"vdd\" value=\"%f\"/><!-- 0 means using ITRS default vdd -->",["L3_vdd","cfg",iCount]])
    template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
    template.append(["\t\t\t\t<param name=\"ports\" value=\"1,1,1\"/>",""])
    template.append(["\t\t\t\t<param name=\"device_type\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<param name=\"buffer_sizes\" value=\"16, 16, 16, 16\"/>",""])
    template.append(["\t\t\t\t<!-- cache controller buffer sizes: miss_buffer_size(MSHR),fill_buffer_size,prefetch_buffer_size,wb_buffer_size-->",""])
    template.append(["\t\t\t\t<stat name=\"read_accesses\" value=\"%i\"/>",["L3.read_accesses","stat",iCount]])                #populate with stats file
    template.append(["\t\t\t\t<stat name=\"write_accesses\" value=\"%i\"/>",["L3.write_accesses","stat",iCount]])               #populate with stats file
    template.append(["\t\t\t\t<stat name=\"read_misses\" value=\"%i\"/>",["L3.read_misses","stat",iCount]])           #populate with stats file
    template.append(["\t\t\t\t<stat name=\"write_misses\" value=\"%i\"/>",["L3.write_misses","stat",iCount]])           #populate with stats file
    template.append(["\t\t\t\t<stat name=\"pic_copy_ops\" value=\"%i\"/>",["L3.pic_copy_ops","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_ops_in_bank\" value=\"%i\"/>",["L3.pic_copy_ops_in_bank","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_tag_access\" value=\"%i\"/>",["L3.pic_copy_tag_access","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_read_miss\" value=\"%i\"/>",["L3.pic_copy_read_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_copy_write_miss\" value=\"%i\"/>",["L3.pic_copy_write_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_ops\" value=\"%i\"/>",["L3.pic_cmp_ops","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_ops_in_bank\" value=\"%i\"/>",["L3.pic_cmp_ops_in_bank","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_tag_access\" value=\"%i\"/>",["L3.pic_cmp_tag_access","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_read_miss\" value=\"%i\"/>",["L3.pic_cmp_read_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_cmp_write_miss\" value=\"%i\"/>",["L3.pic_cmp_write_miss","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"pic_search_key_writes\" value=\"%i\"/>",["L3.pic_key_writes","stat",iCount]])
    template.append(["\t\t\t\t<stat name=\"conflicts\" value=\"0\"/>",""])
    template.append(["\t\t\t\t<stat name=\"duty_cycle\" value=\"%f\"/>",["L3_duty_cycle","stat",iCount]])
    template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------------
##END FOR LOOP

  template.append(["\t\t\t<!--**********************************************************************-->",""])
#template.append(["\t\t<component id=\"system.NoC%i\" name=\"noc%i\">"%(iCount,iCount),""])
  template.append(["\t\t<component id=\"system.NoC0\" name=\"noc0\">",""])
  template.append(["\t\t\t<param name=\"clockrate\" value=\"%i\"/>",["NoC_clock","cfg",iCount]])
  template.append(["\t\t\t<param name=\"vdd\" value=\"%f\"/><!-- 0 means using ITRS default vdd -->",["NoC_vdd","cfg",iCount]])
  template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
#template.append(["\t\t\t<param name=\"clock_rate\" value='%i'/>",["clockFrequency","cfg"]])   #CFG
  template.append(["\t\t\t<param name=\"type\" value=\"%d\"/>",["NoC.type","stat",-1]])
  template.append(["\t\t\t<!--0:bus, 1:NoC , for bus no matter how many nodes sharing the bus at each time only one node can send req -->",""])
  template.append(["\t\t\t<param name=\"horizontal_nodes\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"vertical_nodes\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"has_global_link\" value=\"0\"/>",""])
  template.append(["\t\t\t<param name=\"link_throughput\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"link_latency\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"input_ports\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"output_ports\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"flit_bits\" value=\"256\"/>",""])
  template.append(["\t\t\t<param name=\"chip_coverage\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"link_routing_over_percentage\" value=\"0.5\"/>",""])
  template.append(["\t\t\t<stat name=\"total_accesses\" value=\"%i\"/>",["NoC.total_accesses","stat",-1]])                #populate with stats file
  template.append(["\t\t\t<stat name=\"duty_cycle\" value=\"%f\"/>",["NoC.duty_cycle","stat",-1]])                 #check whether the value is right
  template.append(["\t\t</component>",""])
###################################################################################################################
#------------------------------------------------------------------------------------------------------------------------
  template.append(["\t\t\t<!--**********************************************************************-->",""])
  # system.mem is no longer valid for McPAT 1.0 (it wasn't used anyway, we have our own DRAM power model above)
  '''
  template.append(["\t\t<component id=\"system.mem\" name=\"mem\">",""])
  template.append(["\t\t\t<!-- Main memory property -->",""])
  template.append(["\t\t\t<param name=\"mem_tech_node\" value=\"32\"/>",""])
  template.append(["\t\t\t<param name=\"device_clock\" value=\"200\"/><!--MHz, this is clock rate of the actual memory device, not the FSB -->",""])#check
  template.append(["\t\t\t<param name=\"peak_transfer_rate\" value=\"6400\"/><!--MB/S-->",""])
  template.append(["\t\t\t<param name=\"internal_prefetch_of_DRAM_chip\" value=\"4\"/>",""])    #
  template.append(["\t\t\t<!-- 2 for DDR, 4 for DDR2, 8 for DDR3...-->",""])
  template.append(["\t\t\t<!-- the device clock, peak_transfer_rate, and the internal prefetch decide the DIMM property -->",""])
  template.append(["\t\t\t<!-- above numbers can be easily found from Wikipedia -->",""])
  template.append(["\t\t\t<param name=\"capacity_per_channel\" value=\"4096\"/><!-- MB -->",""])
  template.append(["\t\t\t<!-- capacity_per_Dram_chip=capacity_per_channel/number_of_dimms/number_ranks/Dram_chips_per_rank",""])
  template.append(["\t\t\tCurrent McPAT assumes single DIMMs are used.-->",""])
  template.append(["\t\t\t<param name=\"number_ranks\" value=\"2\"/>",""])
  template.append(["\t\t\t<param name=\"num_banks_of_DRAM_chip\" value=\"8\"/>",""])
  template.append(["\t\t\t<param name=\"Block_width_of_DRAM_chip\" value=\"64\"/><!-- B -->",""])
  template.append(["\t\t\t<param name=\"output_width_of_DRAM_chip\" value=\"8\"/>",""])
  template.append(["\t\t\t<!--number of Dram_chips_per_rank= 72/output_width_of_DRAM_chip-->",""])
  template.append(["\t\t\t<!--number of Dram_chips_per_rank= 72/output_width_of_DRAM_chip-->",""])
  template.append(["\t\t\t<param name=\"page_size_of_DRAM_chip\" value=\"8\"/><!-- 8 or 16 -->",""])    #
  template.append(["\t\t\t<param name=\"burstlength_of_DRAM_chip\" value=\"8\"/>",""])          #
  template.append(["\t\t\t<stat name=\"memory_accesses\" value=\"%i\"/>",["memory.accesses","stat",-1]])         #get from STATS
  template.append(["\t\t\t<stat name=\"memory_reads\" value=\"%i\"/>",["memory.reads","stat",-1]])             #get from STATS
  template.append(["\t\t\t<stat name=\"memory_writes\" value=\"%i\"/>",["memory.writes","stat",-1]])                                                    #get from STATS
  template.append(["\t\t\t</component>",""])
  '''
  template.append(["\t\t<component id=\"system.mc\" name=\"mc\">",""])        #It is required to add this component, however we set number_mcs=0
  template.append(["\t\t\t<!-- current version of McPAT uses published values for base parameters of memory controller",""])
  template.append(["\t\t\timprovments on MC will be added in later versions. -->",""])
  template.append(["\t\t\t<param name=\"mc_clock\" value=\"200\"/><!--MHz-->",""])
  template.append(["\t\t\t<param name=\"vdd\" value=\"0\"/><!-- 0 means using ITRS default vdd -->",""])
  template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
  template.append(["\t\t\t<param name=\"peak_transfer_rate\" value=\"3200\"/>",""])
  template.append(["\t\t\t<param name=\"block_size\" value=\"64\"/><!--B-->",""])
  template.append(["\t\t\t<param name=\"number_mcs\" value=\"0\"/>",""])
  template.append(["\t\t\t<!-- current McPAT only supports homogeneous memory controllers -->",""])
  template.append(["\t\t\t<param name=\"memory_channels_per_mc\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"number_ranks\" value=\"2\"/>",""])
  template.append(["\t\t\t<param name=\"withPHY\" value=\"0\"/>",""])
  template.append(["\t\t\t<param name=\"req_window_size_per_channel\" value=\"32\"/>",""])
  template.append(["\t\t\t<param name=\"IO_buffer_size_per_channel\" value=\"32\"/>",""])
  template.append(["\t\t\t<param name=\"databus_width\" value=\"128\"/>",""])
  template.append(["\t\t\t<param name=\"addressbus_width\" value=\"51\"/>",""])
  template.append(["\t\t\t<!-- McPAT will add the control bus width to the addressbus width automatically -->",""])
  template.append(["\t\t\t<stat name=\"memory_accesses\" value=\"%i\"/>",["memory.accesses","stat",-1]])
  template.append(["\t\t\t<stat name=\"memory_reads\" value=\"%i\"/>",["memory.reads","stat",-1]])
  template.append(["\t\t\t<stat name=\"memory_writes\" value=\"%i\"/>",["memory.writes","stat",-1]])
  template.append(["\t\t\t<!-- McPAT does not track individual mc, instead, it takes the total accesses and calculate ",""])
  template.append(["\t\t\tthe average power per MC or per channel. This is sufficent for most application. ",""])
  template.append(["\t\t\tFurther trackdown can be easily added in later versions. -->",""])
  template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------
  template.append(["\t\t<component id=\"system.niu\" name=\"niu\">",""])
  template.append(["\t\t\t<param name=\"type\" value=\"0\"/> <!-- 1: low power; 0 high performance -->",""])
  template.append(["\t\t\t<param name=\"clockrate\" value=\"350\"/>",""])
  template.append(["\t\t\t<param name=\"vdd\" value=\"0\"/><!-- 0 means using ITRS default vdd -->",""])
  template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
  template.append(["\t\t\t<param name=\"number_units\" value=\"0\"/> <!-- unlike PCIe and memory controllers, each Ethernet controller only have one port -->",""])
  template.append(["\t\t\t<stat name=\"duty_cycle\" value=\"1.0\"/> <!-- achievable max load <= 1.0 -->",""])
  template.append(["\t\t\t<stat name=\"total_load_perc\" value=\"0.7\"/> <!-- ratio of total achived load to total achivable bandwidth  -->",""])
  template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------

  template.append(["\t\t<component id=\"system.pcie\" name=\"pcie\">",""])
  template.append(["\t\t\t<!-- On chip PCIe controller, including Phy-->",""])
  template.append(["\t\t\t<param name=\"type\" value=\"0\"/> <!-- 1: low power; 0 high performance -->",""])
  template.append(["\t\t\t<param name=\"withPHY\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"clockrate\" value=\"350\"/>",""])
  template.append(["\t\t\t<param name=\"vdd\" value=\"0\"/><!-- 0 means using ITRS default vdd -->",""])
  template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
  template.append(["\t\t\t<param name=\"number_units\" value=\"0\"/>",""])
  template.append(["\t\t\t<param name=\"num_channels\" value=\"8\"/> <!-- 2 ,4 ,8 ,16 ,32 -->",""])
  template.append(["\t\t\t<stat name=\"duty_cycle\" value=\"1.0\"/> <!-- achievable max load <= 1.0 -->",""])
  template.append(["\t\t\t<stat name=\"total_load_perc\" value=\"0.7\"/> <!-- Percentage of total achived load to total achivable bandwidth  -->",""])
  template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------
  template.append(["\t\t<component id=\"system.flashc\" name=\"flashc\">",""])
  template.append(["\t\t\t<param name=\"number_flashcs\" value=\"0\"/>",""])
  template.append(["\t\t\t<param name=\"type\" value=\"1\"/> <!-- 1: low power; 0 high performance -->",""])
  template.append(["\t\t\t<param name=\"withPHY\" value=\"1\"/>",""])
  template.append(["\t\t\t<param name=\"peak_transfer_rate\" value=\"200\"/><!--Per controller sustainable reak rate MB/S -->",""])
  template.append(["\t\t\t<param name=\"vdd\" value=\"0\"/><!-- 0 means using ITRS default vdd -->",""])
  template.append(["\t\t\t<param name=\"power_gating_vcc\" value=\"-1\"/><!-- -1 means using default power gating virtual power supply voltage constrained by technology and computed automatically-->",""])
  template.append(["\t\t\t<stat name=\"duty_cycle\" value=\"1.0\"/> <!-- achievable max load <= 1.0 -->",""])
  template.append(["\t\t\t<stat name=\"total_load_perc\" value=\"0.7\"/> <!-- Percentage of total achived load to total achivable bandwidth  -->",""])
  template.append(["\t\t</component>",""])
#------------------------------------------------------------------------------------------------------------------
  template.append(["\t\t<!--**********************************************************************-->",""])
  template.append(["\t</component>",""])
  template.append(["</component>",""])

  return template
#----------
###################################

###################################

if __name__ == '__main__':
  def usage():
    print 'Usage:', sys.argv[0], '[-h (help)] [-j <jobid> | -d <resultsdir (default: .)>] [-t <type: %s>] [-c <override-config>] [-o <output-file (power{.png,.txt,.py})>]' % '|'.join(powertypes)
    sys.exit(-1)

  jobid = 0
  resultsdir = '.'
  powertypes = ['total', 'dynamic', 'static', 'peak', 'peakdynamic', 'area']
  powertype = 'total'
  config = None
  outputfile = 'power'
  no_graph = False
  no_text = False
  partial = None
  no_mcpat_run = False
  bank_map_policies = ['all_ways', 'more_sets']
  bank_map_policy = 'all_ways'
  in_cache_frac = -1.0
  add_implicit_accesses = False
  first_marker = 64

  try:
    opts, args = getopt.getopt(sys.argv[1:], "hij:t:c:d:o:p", [ 'no-graph', 'no-text', 'partial=' , 'no-mcpat-run', 'in-cache-frac=', 'first_marker='])
  except getopt.GetoptError, e:
    print e
    usage()
  for o, a in opts:
    if o == '-h':
      usage()
    if o == '-d':
      resultsdir = a
    if o == '-j':
      jobid = long(a)
    if o == '-t':
      if a not in powertypes:
        sys.stderr.write('Power type %s not supported\n' % a)
        usage()
      powertype = a
    if o == '-p':
      if a not in bank_map_policies:
        sys.stderr.write('Bank mapping policy %s not supported\n' % a)
        usage()
      bank_map_policy = a
    if o == '-c':
      config = a
    if o == '-o':
      outputfile = a
    if o == '--no-graph':
      no_graph = True
    if o == '--no-mcpat-run':
      no_mcpat_run = True
    if o == '-i':
      add_implicit_accesses = True
    if o == '--in-cache-frac':
      in_cache_frac = float(a)
    if o == '--first_marker':
      first_marker = int(a)
    if o == '--no-text':
      no_text = True
    if o == '--partial':
      if ':' not in a:
        sys.stderr.write('--partial=<from>:<to>\n')
        usage()
      partial = a.split(':')


  main(jobid = jobid, resultsdir = resultsdir, powertype = powertype, config = config, outputfile = outputfile, no_graph = no_graph, print_stack = not no_text, partial = partial, bank_map_policy = bank_map_policy, no_mcpat_run = no_mcpat_run, in_cache_frac = in_cache_frac, add_implicit_accesses = add_implicit_accesses, first_marker = first_marker)