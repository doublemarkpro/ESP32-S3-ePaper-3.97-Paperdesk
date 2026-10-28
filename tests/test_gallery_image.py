"""Exercise the firmware JPEG decoder/ditherer with generated JPEG fixtures."""
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
WIDTH,HEIGHT=432,544


class GalleryImageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            from PIL import Image
        except ImportError:
            raise unittest.SkipTest('Pillow is required to generate JPEG fixtures')
        cls.Image=Image
        compiler=shutil.which('g++');c_compiler=shutil.which('gcc')
        if not compiler or not c_compiler:
            raise unittest.SkipTest('g++ and gcc are required for image pipeline tests')
        temporary=tempfile.TemporaryDirectory(prefix='paperdesk-image-test-')
        cls.addClassCleanup(temporary.cleanup)
        cls.folder=Path(temporary.name)
        cls.executable=cls.folder/('gallery_image_test.exe' if os.name=='nt' else 'gallery_image_test')
        vendor=ROOT/'firmware/components/gallery_jpeg'
        commands=[
            [c_compiler,'-O2','-std=c99','-I',str(vendor),'-c',str(vendor/'tjpgd.c'),'-o',str(cls.folder/'jpeg.o')],
            [compiler,'-O2','-std=c++17','-Wall','-Wextra','-Werror','-finput-charset=UTF-8','-fexec-charset=UTF-8',
             '-I',str(vendor),'-I',str(ROOT/'firmware/main'),str(ROOT/'tests/gallery_image_test.cpp'),
             str(ROOT/'firmware/main/gallery_image.cpp'),str(cls.folder/'jpeg.o'),'-o',str(cls.executable)]
        ]
        for command in commands:
            result=subprocess.run(command,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60)
            if result.returncode:raise AssertionError(result.stdout+result.stderr)

    def jpeg(self,size,color,mode='L',progressive=False):
        stream=io.BytesIO();self.Image.new(mode,size,color).save(stream,format='JPEG',quality=95,progressive=progressive)
        return stream.getvalue()

    def decode(self,data,success=True):
        source=self.folder/'input.jpg';target=self.folder/'output.bin';source.write_bytes(data)
        result=subprocess.run([str(self.executable),str(source),str(target)],capture_output=True,timeout=20)
        self.assertEqual(result.returncode,0 if success else 2,result.stderr.decode('utf-8',errors='replace'))
        if success:
            output=target.read_bytes();self.assertEqual(len(output),WIDTH*HEIGHT//8)
            return self.Image.frombytes('1',(WIDTH,HEIGHT),output)

    def test_white_stays_white_including_margins(self):
        self.assertEqual(self.decode(self.jpeg((500,800),255)).getextrema(),(255,255))

    def test_black_landscape_is_centered_without_cropping(self):
        image=self.decode(self.jpeg((400,200),0)).convert('L')
        bbox=image.point(lambda x:255-x).getbbox()
        self.assertEqual(bbox,(0,164,432,380))

    def test_black_portrait_is_centered_without_cropping(self):
        image=self.decode(self.jpeg((200,400),0)).convert('L')
        self.assertEqual(image.point(lambda x:255-x).getbbox(),(80,0,352,544))

    def test_gray_is_dithered_not_thresholded_to_a_solid_fill(self):
        image=self.decode(self.jpeg((432,544),128))
        white=image.histogram()[-1]/(WIDTH*HEIGHT)
        self.assertGreater(white,0.49);self.assertLess(white,0.52)

    def test_rgb_jpeg_uses_luminance(self):
        image=self.decode(self.jpeg((432,544),(255,0,0),'RGB'))
        white=image.histogram()[-1]/(WIDTH*HEIGHT)
        self.assertGreater(white,0.27);self.assertLess(white,0.33)

    def test_large_and_odd_dimensions_decode(self):
        for size in ((3000,1500),(513,1027),(1,4096),(1,1)):
            with self.subTest(size=size):self.decode(self.jpeg(size,255))

    def test_gradient_keeps_both_bright_and_dark_detail(self):
        image=self.Image.new('L',(432,544))
        image.putdata([round(x*255/431) for y in range(544) for x in range(432)])
        stream=io.BytesIO();image.save(stream,format='JPEG',quality=95)
        result=self.decode(stream.getvalue())
        left=result.crop((0,0,60,544)).histogram()[-1]/(60*544)
        right=result.crop((372,0,432,544)).histogram()[-1]/(60*544)
        self.assertLess(left,0.1);self.assertGreater(right,0.9)

    def test_paper_white_is_lifted_while_dark_ink_remains(self):
        image=self.Image.new('L',(432,544),236)
        # A broad dark ink patch establishes a meaningful tonal range.
        image.paste(50,(100,100,300,350))
        stream=io.BytesIO();image.save(stream,format='JPEG',quality=95)
        result=self.decode(stream.getvalue())
        paper=result.crop((10,10,80,530)).histogram()[-1]/(70*520)
        ink=result.crop((120,120,280,330)).histogram()[-1]/(160*210)
        self.assertGreater(paper,0.975);self.assertLess(ink,0.25)

    def test_flat_light_gray_is_not_whitened(self):
        image=self.decode(self.jpeg((432,544),236))
        white=image.histogram()[-1]/(WIDTH*HEIGHT)
        self.assertGreater(white,0.91);self.assertLess(white,0.94)

    def test_dark_photo_range_is_expanded_without_solid_black(self):
        image=self.Image.new('L',(432,544))
        image.putdata([20+round(x*110/431) for y in range(544) for x in range(432)])
        stream=io.BytesIO();image.save(stream,format='JPEG',quality=95)
        result=self.decode(stream.getvalue())
        left=result.crop((0,0,60,544)).histogram()[-1]/(60*544)
        right=result.crop((372,0,432,544)).histogram()[-1]/(60*544)
        self.assertGreater(left,0.05);self.assertLess(left,0.2);self.assertGreater(right,0.9)

    def test_truncated_or_non_jpeg_preserves_old_image(self):
        data=self.jpeg((400,600),0)
        for bad in (b'',b'<html>unavailable</html>',data[:-2],data[:len(data)//2],data[:100]+b'\xff\xd9'):
            with self.subTest(length=len(bad)):self.decode(bad,False)

    def test_progressive_and_oversize_images_preserve_old_image(self):
        for bad in (self.jpeg((432,544),128,progressive=True),self.jpeg((4097,8),0),b'\xff\xd8'+b'\x00'*(768*1024)+b'\xff\xd9'):
            with self.subTest(length=len(bad)):self.decode(bad,False)

    def test_malformed_frame_and_scan_headers_preserve_old_image(self):
        original=self.jpeg((432,544),0)
        for marker in (b'\xff\xc0',b'\xff\xda'):
            bad=bytearray(original);offset=bad.index(marker)
            bad[offset+2:offset+4]=b'\x00\x03'
            with self.subTest(marker=marker):self.decode(bad,False)


if __name__=='__main__':unittest.main()
