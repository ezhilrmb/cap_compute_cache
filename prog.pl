#!/usr/bin/perl
use strict;
use warnings;

# cache parameters
my $cache_line_bit_width = 512;
my $single_SA_num_lines = 256;
my $num_subarrays = 2;

# swizzle switch parameters
my $swizzle_switch_x = 512*$num_subarrays;
my $swizzle_switch_y = 512*$num_subarrays;

my $file = 'snort1.anml';
my @STE_id;
my $STE_id_ctr = 0;
my @symbol_set;
my $symbol_set_ctr = 0;
my @start_of_data;
my $start_of_data_ctr = 0;

my %symbol_to_STE_map;
my %swizzle_map;
my %reporting_STE;
my $random_ctr=0;
my $temp_ctr=0;
my @temp_array;

my $prev_char = '';
my $next_char = '';
my $curr_char = '';

my %char_classes = ('s' => ['32','9','13','10','12'],
					'd' => ['48','49','50','51','52','53','54','55','56','57'],
					'=' => ['61']);

my $id;
my $id1;
open my $info, $file or die "Could not open $file";

while(my $line = <$info>){

	# creating a list of all STE ids #
	if($line =~ /(state-transition-element id="__)(.*)(__")/){
		$STE_id[$STE_id_ctr] = $2;
		$STE_id_ctr++;
	}
	if($line =~ /(or id="__)(.*)(__")/){
		$STE_id[$STE_id_ctr] = $2;
		$STE_id_ctr++;
	}

	# swizzle switch mapping #
	if($line =~ /(activate-on-match element="__)(.*)(__)/){
		push @{$swizzle_map{$STE_id[$STE_id_ctr-1]}},$2;
	}

	# symbol sets #
	# assuming a one to one mapping of symbol set to STE id(not true for "or id") #
	if ($line =~ /(symbol-set="\[)(.*)(\]")/){
		$symbol_set[$symbol_set_ctr] = "\Q$2\E";
		$symbol_set_ctr++;
	}

	# marker to indicate if an STE id is a start STE #
	if($line =~ /("start-of-data")/){

		$start_of_data[$start_of_data_ctr] = $STE_id[$STE_id_ctr-1];
		$start_of_data_ctr++;
	}

	# marker to indicate if an STE id is a reporting STE #
	if($line =~ /(reportcode=")(.*)(")/){
		push @{$reporting_STE{$STE_id[$STE_id_ctr-1]}},$2;
	}	
}

# foreach $id (<@STE_id>){
# 	print "$id\n";
# }

# print "----------- \n";

# foreach $id (<@symbol_set>){
# 	print "$id \n";
# }

# print "----------- \n";

#print "Start of data STEs\n";
# foreach $id (<@start_of_data>){
# 	print "$id\n";
# }
#
# print "\n\n";

# print "----------- \n";

# foreach $id (keys %swizzle_map)
# {
#   @temp_array =  @{$swizzle_map{$id}};
#   print "$id ","@temp_array","\n";
# }

#print "Reporting STEs...\n";
# foreach $id (keys %reporting_STE)
# {
#   @temp_array =  @{$reporting_STE{$id}};
#   print "$id ","@temp_array","\n";
# }

while ($random_ctr<$symbol_set_ctr){
	if($symbol_set[$random_ctr] =~ /(\\x)(.{2})/){ # for the unicode hex characters
		push @{ $symbol_to_STE_map{hex($2)} },$STE_id[$random_ctr];
		$symbol_set[$random_ctr] =~ s/\\\\x.{2}//;
	}
	if($symbol_set[$random_ctr] =~ /(\\s)/){
		print "FOUND \\s ";
		foreach $id (@{$char_classes{"\\s"}}){
			push @{ $symbol_to_STE_map{$id} },$STE_id[$random_ctr];
		}
	}
	if($symbol_set[$random_ctr] =~ /(\\d)/){
		#print "FOUND \\d ";
		foreach $id (@{$char_classes{"\\d"}}){
			push @{ $symbol_to_STE_map{$id} },$STE_id[$random_ctr];
		}
	}
	if($symbol_set[$random_ctr] =~ /(.)(\\)(-)(.)/){
		#print "$symbol_set[$random_ctr]\n";
		for ($id=ord($1);$id<=ord($4);$id++){
			push @{ $symbol_to_STE_map{$id} },$STE_id[$random_ctr];
		}
		$symbol_set[$random_ctr] =~ s/(.)(\\)(-)(.)//;
	}

	for ($id=0;$id < length $symbol_set[$random_ctr];$id++){
		$curr_char = substr($symbol_set[$random_ctr], $id, 1);
		
		if($id == 0){
			$prev_char = '';
		}
		else{
			$prev_char = substr($symbol_set[$random_ctr], $id-1, 1);
		}

		if($id == (length $symbol_set[$random_ctr])-1){
			$next_char = '';
		}
		else{
			$next_char = substr("$symbol_set[$random_ctr]", $id+1, 1);
		}

		if($curr_char eq "\\"){
			if($next_char ne "\\"){
				foreach $id1 (@{$char_classes{$next_char}}){
					#print "$id1\t";
					push @{ $symbol_to_STE_map{$id1} },$STE_id[$random_ctr];
				}
			}
		}
		elsif($prev_char ne "\\" and $prev_char ne "\-" and $next_char ne "\-"){
			push @{ $symbol_to_STE_map{ord($curr_char)} },$STE_id[$random_ctr];
		}
	}

	#print "\n";

 	$random_ctr++;
}

#print "Symbol to STE map:\n";
# foreach $id (keys %symbol_to_STE_map)
# {
#   @temp_array =  @{$symbol_to_STE_map{$id}};
#   print "$id ","@temp_array","\n";
# }

my @cache_array;
my $loop_ctr;
my $subId = 0;

for ($subId=0; $subId<$num_subarrays; $subId++) {
   for ($id=0;$id<$single_SA_num_lines;$id++){
       for ($loop_ctr=0;$loop_ctr<$cache_line_bit_width;$loop_ctr++){
           $cache_array[$subId][$id][$loop_ctr] = '0';
       }
   }
}

# 2D ARRAY FOR THE CACHE PROGRAMMING #
foreach $id (keys %symbol_to_STE_map)
{	
	@temp_array =  @{$symbol_to_STE_map{$id}};
	foreach $loop_ctr (<@temp_array>){
       {
          use integer;
          $subId = $loop_ctr/$cache_line_bit_width;
       }
       # print "symbol: $id, pos: $loop_ctr, subId =  $subId, ";
       $loop_ctr = $loop_ctr % $cache_line_bit_width;
       # print "stored at: $loop_ctr\n";
       $cache_array[$subId][$id][$loop_ctr] = '1';
	}
}

# Cache programming file gen
my $decimal = 0;
my $character;
my $filename = 'cachep.txt';
my $dummy = 199;
open(my $fh, '>', $filename);

for ($subId=0; $subId<$num_subarrays; $subId++) {
   for ($id=0;$id<$single_SA_num_lines;$id++){
          for ($random_ctr=0;$random_ctr<$cache_line_bit_width/8;$random_ctr++)  {
              for ($loop_ctr=0;$loop_ctr<8;$loop_ctr++){
                  # print $fh "$cache_array[$id][8*$random_ctr + $loop_ctr]";
                  $decimal = $decimal | ($cache_array[$subId][$id][8*$random_ctr + $loop_ctr] << (7-$loop_ctr));
                  #$decimal = $decimal + (2**(7-$loop_ctr))*(ord($cache_array[$id][8*$random_ctr + $loop_ctr])-48);
              }
              if($decimal==0){
                  $character = sprintf("%c",$dummy);	
              }
              else{
                  $character = sprintf("%c",$decimal);	
              }
              print $fh $character;
              #print $fh "\n";
              $decimal = 0;
          }
          #print $fh "\n";
   }
}
close $fh;

=pods
$decimal = 0;
my $filename = 'cache2.txt';
open(my $fh, '>', $filename);
for ($id=0;$id<256;$id++){
	for ($random_ctr= 64;$random_ctr<1024/8;$random_ctr++){
		for ($loop_ctr=0;$loop_ctr<8;$loop_ctr++){
			#print $fh "$cache_array[$id][8*$random_ctr + $loop_ctr]";
			$decimal = $decimal + (2**(7-$loop_ctr))*(ord($cache_array[$id][8*$random_ctr + $loop_ctr])-48);
		}
		if($decimal==0){
			$character = sprintf("%c",$dummy);	
		}
		else{
			$character = sprintf("%c",$decimal);	
		}
		print $fh $character;
		#print $fh "\n";
		$decimal = 0;
	}
	#print $fh "\n";
}
close $fh;
=cut

# Swizzle switch file gen
my @swizzle_array;

for ($id=0;$id<$swizzle_switch_x;$id++){
	for ($loop_ctr=0;$loop_ctr<$swizzle_switch_y;$loop_ctr++){
		$swizzle_array[$id][$loop_ctr] = '0';
	}
}
foreach $id (keys %swizzle_map)
{	
	@temp_array =  @{$swizzle_map{$id}};
	foreach $loop_ctr (<@temp_array>){
		$swizzle_array[$id][$loop_ctr] = '1';
	}
}

$decimal = 0;
$filename = 'ssp.txt';
open($fh, '>', $filename);
for ($id=0;$id<$swizzle_switch_x;$id++){
	for ($random_ctr=0;$random_ctr<$swizzle_switch_y/8;$random_ctr++){
		for ($loop_ctr=0;$loop_ctr<8;$loop_ctr++){
			#print $fh "$swizzle_array[$id][8*$random_ctr + $loop_ctr]";
            $decimal = $decimal | ($swizzle_array[$id][8*$random_ctr + $loop_ctr] << (7-$loop_ctr));
            #$decimal = $decimal + (2**(7-$loop_ctr))*(ord($swizzle_array[$id][8*$random_ctr + $loop_ctr])-48);
		}
		if($decimal==0){
			$character = sprintf("%c",$dummy);	
		}
		else{
			$character = sprintf("%c",$decimal);	
		}
		print $fh $character;
		#print $fh "\n";
		$decimal = 0;
	}
	#print $fh "\n";
}
close $fh;

# start STE file gen
my %startSteHash = map { $_ => 1 } @start_of_data;
my @startSteArray;

for ($id=0; $id<$cache_line_bit_width*$num_subarrays; $id++) {
   if (exists $startSteHash{$id})  {
      $startSteArray[$id] = 1;
   }
   else {
      $startSteArray[$id] = 0;
   }
}

$decimal = 0;
$filename = 'repSTE.txt'; # this file holds start and end STE mask information
open($fh, '>', $filename);

for ($random_ctr=0;$random_ctr<$cache_line_bit_width*$num_subarrays/8;$random_ctr++){
	for ($loop_ctr=0;$loop_ctr<8;$loop_ctr++){
        $decimal = $decimal | ($startSteArray[8*$random_ctr + $loop_ctr] << (7-$loop_ctr));
	}
	if($decimal==0){
		$character = sprintf("%c",$dummy);	
	}
	else{
		$character = sprintf("%c",$decimal);	
	}
	print $fh $character;
	$decimal = 0;
}

# reporting STE file gen
my @repSteArray;

for ($id=0; $id<$cache_line_bit_width*$num_subarrays; $id++) {
   if (exists $reporting_STE{$id})  {
      $repSteArray[$id] = 1;
   }
   else {
      $repSteArray[$id] = 0;
   }
}

$decimal = 0;

	for ($random_ctr=0;$random_ctr<$cache_line_bit_width*$num_subarrays/8;$random_ctr++){
		for ($loop_ctr=0;$loop_ctr<8;$loop_ctr++){
            $decimal = $decimal | ($repSteArray[8*$random_ctr + $loop_ctr] << (7-$loop_ctr));
		}
		if($decimal==0){
			$character = sprintf("%c",$dummy);	
		}
		else{
			$character = sprintf("%c",$decimal);	
		}
		print $fh $character;
		$decimal = 0;
	}
close $fh;


close $info;
