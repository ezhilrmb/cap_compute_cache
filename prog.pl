use strict;

my $file = 'snort1.anml';
my @STE_id;
my $STE_id_ctr = 0;
my @symbol_set;
my $symbol_set_ctr = 0;
my @start_of_data;
my $start_of_data_ctr;

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
		@STE_id[$STE_id_ctr] = $2;
		$STE_id_ctr++;
	}
	if($line =~ /(or id="__)(.*)(__")/){
		@STE_id[$STE_id_ctr] = $2;
		$STE_id_ctr++;
	}

	# swizzle switch mapping #
	if($line =~ /(activate-on-match element="__)(.*)(__)/){
		push @{$swizzle_map{$STE_id[$STE_id_ctr-1]}},$2;
	}

	# symbol sets #
	# assuming a one to one mapping of symbol set to STE id(not true for "or id") #
	if ($line =~ /(symbol-set="\[)(.*)(\]")/){
		@symbol_set[$symbol_set_ctr] = "\Q$2\E";
		$symbol_set_ctr++;
	}

	# marker to indicate if an STE id is a start STE #
	if($line =~ /("start-of-data")/){
		@start_of_data[$start_of_data_ctr] = @STE_id[$STE_id_ctr-1];
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

# foreach $id (<@start_of_data>){
# 	print "$id\n";
# }

# print "----------- \n";

# foreach $id (keys %swizzle_map)
# {
#   @temp_array =  @{$swizzle_map{$id}};
#   print "$id ","@temp_array","\n";
# }

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

# foreach $id (keys %symbol_to_STE_map)
# {
#   @temp_array =  @{$symbol_to_STE_map{$id}};
#   print "$id ","@temp_array","\n";
# }

my @cache_array;
my $loop_ctr;
for ($id=0;$id<256;$id++){
	for ($loop_ctr=0;$loop_ctr<1024;$loop_ctr++){
		$cache_array[$id][$loop_ctr] = '0';
	}
}
# 2D ARRAY FOR THE CACHE PROGRAMMING #
foreach $id (keys %symbol_to_STE_map)
{	
	@temp_array =  @{$symbol_to_STE_map{$id}};
	foreach $loop_ctr (<@temp_array>){
		$cache_array[$id][$loop_ctr] = '1';
	}
}

my $decimal = 0;
my $character;
my $filename = 'cache1.txt';
my $dummy = 199;
open(my $fh, '>', $filename);
for ($id=0;$id<256;$id++){
	for ($random_ctr=0;$random_ctr<512/8;$random_ctr++){
		for ($loop_ctr=0;$loop_ctr<8;$loop_ctr++){
			# print $fh "$cache_array[$id][8*$random_ctr + $loop_ctr]";
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

my @swizzle_array;
for ($id=0;$id<1024;$id++){
	for ($loop_ctr=0;$loop_ctr<1024;$loop_ctr++){
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
$filename = 'swizzle.txt';
open($fh, '>', $filename);
for ($id=0;$id<1024;$id++){
	for ($random_ctr=0;$random_ctr<1024/8;$random_ctr++){
		for ($loop_ctr=0;$loop_ctr<8;$loop_ctr++){
			#print $fh "$swizzle_array[$id][8*$random_ctr + $loop_ctr]";
			$decimal = $decimal + (2**(7-$loop_ctr))*(ord($swizzle_array[$id][8*$random_ctr + $loop_ctr])-48);
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
close $info;
