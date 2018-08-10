Impala-lzo
==========

This project provides support for reading LZO compressed tables from Impala.

Generally you should also install the Hadoop-lzo project which provides support for indexing the files produced by the lzop program.

Tables containing lzo compressed files must be created in Hive with:
  stored as
  INPUTFORMAT 'com.hadoop.mapred.DeprecatedLzoTextInputFormat'
  OUTPUTFORMAT 'org.apache.hadoop.hive.ql.io.HiveIgnoreKeyTextOutputFormat'

To build the library do:
  You must set the environment variable IMPALA_HOME to the root of an Impala development
tree.
  cmake .
  make
at the top level will put the resulting libimpalalzo.so in the build directory.  This file should be moved to ${IMPALA_HOME}/lib/. OR any directory that is in the LD_LIBRARY_PATH of your running impalad servers.

# How do I contribute code?
You need to first sign and return an
[ICLA](https://github.com/cloudera/native-toolchain/blob/icla/Cloudera%20ICLA_25APR2018.pdf)
and
[CCLA](https://github.com/cloudera/native-toolchain/blob/icla/Cloudera%20CCLA_25APR2018.pdf)
before we can accept and redistribute your contribution. Once these are submitted you are
free to start contributing to Impala-lzo. Submit these to CLA@cloudera.com.

## Find
We use Github issues to track bugs for this project. Find an issue that you would like to
work on (or file one if you have discovered a new issue!). If no-one is working on it,
assign it to yourself only if you intend to work on it shortly.

It’s a good idea to discuss your intended approach on the issue. You are much more
likely to have your patch reviewed and committed if you’ve already got buy-in from the
Impala-lzo community before you start.

## Fix
Now start coding! As you are writing your patch, please keep the following things in mind:

First, please include tests with your patch. If your patch adds a feature or fixes a bug
and does not include tests, it will generally not be accepted. If you are unsure how to
write tests for a particular component, please ask on the issue for guidance.

Second, please keep your patch narrowly targeted to the problem described by the issue.
It’s better for everyone if we maintain discipline about the scope of each patch. In
general, if you find a bug while working on a specific feature, file a issue for the bug,
check if you can assign it to yourself and fix it independently of the feature. This helps
us to differentiate between bug fixes and features and allows us to build stable
maintenance releases.

Finally, please write a good, clear commit message, with a short, descriptive title and
a message that is exactly long enough to explain what the problem was, and how it was
fixed.

Please create a pull request on github with your patch.
