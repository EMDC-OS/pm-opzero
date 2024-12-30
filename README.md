# PM OPZero

## Motivation
DAX file systems, designed to leverage the low latency and byte-addressability of persistent memory, bypass the page cache layer, enhancing performance. However, this study uncovers that data block allocation in DAX file systems is considerably slower than in conventional file systems, primarily due to the mandatory zero-out operation for newly allocated blocks, intended to prevent old data leakage. This leads to significant implications for file write performance, which our research aims to address.

![motiv (1)](https://github.com/EMDC-OS/pm-opzero/assets/17569303/75195716-fec9-43a8-b9f9-a784937826cb)

## Design
Our approach introduces an off-critical-path data block sanitization scheme specifically for DAX file systems. This scheme detaches the zero-out operation from the latency-critical I/O path and conducts it in the background, focusing on the released data blocks. The key design principle is universally applicable across most DAX file systems. The implementation of this approach in Ext4-DAX and XFS-DAX demonstrates notable improvements in performance.

![short-design-1](https://github.com/EMDC-OS/pm-opzero/assets/17569303/dd7cbc9f-84f5-43e6-95cb-c807a0d36e68)

## Usage
If you wish to use ext4, you should download the ext4 branch, and if you prefer to use xfs, you should download the xfs branch. After building and installing the kernel you received, you can start the sanitization process in the background as follows:

Adjust appropriately to either ext4 or xfs depending on your desired filesystem.
```
git checkout xfs
# or
git checkout ext4
```

```
# The directory name for free_inode and the name for frinode_value may vary depending on the situation, but they both start with 'free,' and they both should start with '1.
echo 1 > /sys/kernel/free_inode/frinode_value
```

## Citation
```
@inproceedings{park2022analysis,
  title={Analysis and Mitigation of Data Sanitization Overhead in {DAX} File Systems},
  author={Park, Soyoung and Kim, Jongseok and Lim, Younghoon and Seo, Euiseong},
  booktitle={2022 IEEE 40th International Conference on Computer Design (ICCD '22)},
  pages={255--258},
  year={2022},
  organization={IEEE}
}
```
